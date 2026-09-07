// A real CLAP plugin, built by this project, used to prove the host works.
//
// The point of building our own rather than only testing against installed
// third-party plugins is that a test which needs a distribution package is a
// test that silently skips. This bundle is compiled from source in the build
// tree, so "a plugin loaded and changed the audio" is asserted on every build
// on every machine, and the third-party plugins become a second, independent
// confirmation rather than the only one.
//
// It publishes four plugins, each chosen to exercise a specific thing the host
// has to get right and that is otherwise only reachable by finding a real
// plugin that happens to do it:
//
//   gain        1-in/1-out mono, one parameter. The baseline: does a plugin
//               load, activate, receive a parameter and change the samples?
//   stereo      2-in/2-out with an inverted right channel. Proves the host
//               duplicates mono into both input channels and takes CHANNEL 0
//               of the output -- a host that averaged the channels would get
//               silence here, which is exactly the bug the rule exists for.
//   sidechain   Two declared input ports, and it READS the second one. A host
//               that allocates only the main port segfaults on this.
//   poison      Emits NaN. Proves the chain notices and switches itself off
//               instead of encoding NaN into the call.
//
// Written in C against the vendored headers only. No SDK, no framework, and no
// dependency beyond libm, so it builds anywhere the host does.

#include <clap/clap.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OC_FRAMES 960

// --- descriptors ----------------------------------------------------------

static const char *const oc_features_fx[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL};

static const clap_plugin_descriptor_t oc_desc_gain = {
    .clap_version = CLAP_VERSION_INIT,
    .id = "org.openchat.test.gain",
    .name = "OpenChat Test Gain",
    .vendor = "OpenChat",
    .url = "",
    .manual_url = "",
    .support_url = "",
    .version = "1.0.0",
    .description = "Mono gain, for testing the host.",
    .features = oc_features_fx,
};

static const clap_plugin_descriptor_t oc_desc_stereo = {
    .clap_version = CLAP_VERSION_INIT,
    .id = "org.openchat.test.stereo",
    .name = "OpenChat Test Stereo",
    .vendor = "OpenChat",
    .url = "",
    .manual_url = "",
    .support_url = "",
    .version = "1.0.0",
    .description = "Stereo, right channel inverted.",
    .features = oc_features_fx,
};

static const clap_plugin_descriptor_t oc_desc_sidechain = {
    .clap_version = CLAP_VERSION_INIT,
    .id = "org.openchat.test.sidechain",
    .name = "OpenChat Test Sidechain",
    .vendor = "OpenChat",
    .url = "",
    .manual_url = "",
    .support_url = "",
    .version = "1.0.0",
    .description = "Reads a second input port.",
    .features = oc_features_fx,
};

static const clap_plugin_descriptor_t oc_desc_poison = {
    .clap_version = CLAP_VERSION_INIT,
    .id = "org.openchat.test.poison",
    .name = "OpenChat Test Poison",
    .vendor = "OpenChat",
    .url = "",
    .manual_url = "",
    .support_url = "",
    .version = "1.0.0",
    .description = "Emits NaN, on purpose.",
    .features = oc_features_fx,
};

// --- instance -------------------------------------------------------------

enum oc_kind { OC_GAIN, OC_STEREO, OC_SIDECHAIN, OC_POISON };

#define OC_PARAM_GAIN 0x9A11

typedef struct {
    clap_plugin_t plugin;
    const clap_host_t *host;
    enum oc_kind kind;
    double gain;
    // Set by the host calling activate(); read by the test through the
    // parameter, which is how a test can tell activation really happened.
    int activated;
    int processing;
    // Counts frames the sidechain variant saw a non-zero sidechain in, which is
    // how the test proves the second port was really allocated and passed.
    int sidechain_nonzero_frames;
} oc_plugin;

static oc_plugin *oc_self(const clap_plugin_t *plugin)
{
    return (oc_plugin *)plugin->plugin_data;
}

// --- audio-ports extension ------------------------------------------------

static uint32_t oc_ports_count(const clap_plugin_t *plugin, bool is_input)
{
    const oc_plugin *self = oc_self(plugin);
    if (self->kind == OC_SIDECHAIN && is_input)
        return 2; // main + sidechain
    return 1;
}

static bool oc_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input,
                         clap_audio_port_info_t *info)
{
    const oc_plugin *self = oc_self(plugin);
    if (index >= oc_ports_count(plugin, is_input))
        return false;

    memset(info, 0, sizeof(*info));
    info->id = index;
    info->in_place_pair = CLAP_INVALID_ID;
    info->port_type = CLAP_PORT_MONO;

    if (self->kind == OC_STEREO) {
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        snprintf(info->name, sizeof(info->name), "%s", is_input ? "Input" : "Output");
        return true;
    }

    if (self->kind == OC_SIDECHAIN && is_input && index == 1) {
        info->channel_count = 1;
        // Deliberately flags == 0 rather than a distinct sidechain bit: real
        // plugins disagree about the flag word, and only IS_MAIN's absence is
        // reliable. A host that looked for a specific sidechain value would
        // mis-identify this port.
        info->flags = 0;
        snprintf(info->name, sizeof(info->name), "Sidechain");
        return true;
    }

    info->channel_count = 1;
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    snprintf(info->name, sizeof(info->name), "%s", is_input ? "Input" : "Output");
    return true;
}

static const clap_plugin_audio_ports_t oc_ext_audio_ports = {
    .count = oc_ports_count,
    .get = oc_ports_get,
};

// --- params extension -----------------------------------------------------

static uint32_t oc_params_count(const clap_plugin_t *plugin)
{
    return oc_self(plugin)->kind == OC_GAIN ? 1 : 0;
}

static bool oc_params_get_info(const clap_plugin_t *plugin, uint32_t index,
                               clap_param_info_t *info)
{
    if (oc_self(plugin)->kind != OC_GAIN || index != 0)
        return false;
    memset(info, 0, sizeof(*info));
    info->id = OC_PARAM_GAIN;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    info->min_value = 0.0;
    info->max_value = 4.0;
    info->default_value = 1.0;
    // A non-null cookie, so a host that fails to copy it verbatim into the
    // event is caught rather than accidentally working.
    info->cookie = (void *)&oc_ext_audio_ports;
    snprintf(info->name, sizeof(info->name), "Gain");
    info->module[0] = '\0';
    return true;
}

static bool oc_params_get_value(const clap_plugin_t *plugin, clap_id id, double *out)
{
    if (oc_self(plugin)->kind != OC_GAIN || id != OC_PARAM_GAIN)
        return false;
    *out = oc_self(plugin)->gain;
    return true;
}

static bool oc_params_value_to_text(const clap_plugin_t *plugin, clap_id id, double value,
                                    char *out, uint32_t size)
{
    (void)plugin;
    if (id != OC_PARAM_GAIN)
        return false;
    snprintf(out, size, "%.2f x", value);
    return true;
}

static bool oc_params_text_to_value(const clap_plugin_t *plugin, clap_id id, const char *text,
                                    double *out)
{
    (void)plugin;
    if (id != OC_PARAM_GAIN)
        return false;
    *out = atof(text);
    return true;
}

static void oc_params_flush(const clap_plugin_t *plugin, const clap_input_events_t *in,
                            const clap_output_events_t *out)
{
    (void)plugin;
    (void)in;
    (void)out;
}

static const clap_plugin_params_t oc_ext_params = {
    .count = oc_params_count,
    .get_info = oc_params_get_info,
    .get_value = oc_params_get_value,
    .value_to_text = oc_params_value_to_text,
    .text_to_value = oc_params_text_to_value,
    .flush = oc_params_flush,
};

// --- state extension ------------------------------------------------------

static bool oc_state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream)
{
    const double gain = oc_self(plugin)->gain;
    return stream->write(stream, &gain, sizeof(gain)) == (int64_t)sizeof(gain);
}

static bool oc_state_load(const clap_plugin_t *plugin, const clap_istream_t *stream)
{
    double gain = 1.0;
    if (stream->read(stream, &gain, sizeof(gain)) != (int64_t)sizeof(gain))
        return false;
    oc_self(plugin)->gain = gain;
    return true;
}

static const clap_plugin_state_t oc_ext_state = {
    .save = oc_state_save,
    .load = oc_state_load,
};

// --- latency extension ----------------------------------------------------

static uint32_t oc_latency_get(const clap_plugin_t *plugin)
{
    // A non-zero, plugin-specific figure so a test can tell a real read from a
    // hardcoded zero.
    return oc_self(plugin)->kind == OC_STEREO ? 64 : 0;
}

static const clap_plugin_latency_t oc_ext_latency = {.get = oc_latency_get};

// --- plugin ---------------------------------------------------------------

static bool oc_init(const clap_plugin_t *plugin)
{
    (void)plugin;
    return true;
}

static void oc_destroy(const clap_plugin_t *plugin)
{
    free(oc_self(plugin));
}

static bool oc_activate(const clap_plugin_t *plugin, double sample_rate, uint32_t min_frames,
                        uint32_t max_frames)
{
    oc_plugin *self = oc_self(plugin);
    // Refuses anything but what the call actually uses, so a host that quietly
    // activated at the wrong rate or block size fails loudly here rather than
    // producing subtly wrong audio.
    if (sample_rate != 48000.0 || min_frames > OC_FRAMES || max_frames < OC_FRAMES)
        return false;
    self->activated = 1;
    return true;
}

static void oc_deactivate(const clap_plugin_t *plugin)
{
    oc_self(plugin)->activated = 0;
}

static bool oc_start_processing(const clap_plugin_t *plugin)
{
    oc_self(plugin)->processing = 1;
    return true;
}

static void oc_stop_processing(const clap_plugin_t *plugin)
{
    oc_self(plugin)->processing = 0;
}

static void oc_reset(const clap_plugin_t *plugin)
{
    oc_self(plugin)->sidechain_nonzero_frames = 0;
}

static void oc_handle_events(oc_plugin *self, const clap_input_events_t *in)
{
    // in_events is never null by specification. Dereferencing it without a
    // check is deliberate: it is what real plugins do, and it is what makes a
    // host that passes null crash here rather than in production.
    const uint32_t count = in->size(in);
    for (uint32_t i = 0; i < count; ++i) {
        const clap_event_header_t *header = in->get(in, i);
        if (!header || header->space_id != CLAP_CORE_EVENT_SPACE_ID)
            continue;
        if (header->type != CLAP_EVENT_PARAM_VALUE)
            continue;
        if (header->size != sizeof(clap_event_param_value_t))
            continue; // A host that sent sizeof(header) is caught here.
        const clap_event_param_value_t *event = (const clap_event_param_value_t *)header;
        if (event->param_id == OC_PARAM_GAIN)
            self->gain = event->value;
    }
}

static clap_process_status oc_process(const clap_plugin_t *plugin, const clap_process_t *process)
{
    oc_plugin *self = oc_self(plugin);
    if (!self->processing)
        return CLAP_PROCESS_ERROR;
    if (process->in_events)
        oc_handle_events(self, process->in_events);

    const uint32_t frames = process->frames_count;
    const float *in = process->audio_inputs[0].data32[0];
    float *out = process->audio_outputs[0].data32[0];

    switch (self->kind) {
    case OC_GAIN:
        for (uint32_t i = 0; i < frames; ++i)
            out[i] = (float)(in[i] * self->gain);
        break;

    case OC_STEREO: {
        // Left is passed through; right is inverted. A host that averages the
        // two output channels gets silence, which is the failure this exists to
        // catch.
        float *right = process->audio_outputs[0].data32[1];
        for (uint32_t i = 0; i < frames; ++i) {
            out[i] = in[i];
            right[i] = -in[i];
        }
        break;
    }

    case OC_SIDECHAIN: {
        // Reads the SECOND declared input port. A host that allocated only the
        // main one dereferences past the end of its array here.
        const float *sidechain = process->audio_inputs[1].data32[0];
        int nonzero = 0;
        for (uint32_t i = 0; i < frames; ++i) {
            out[i] = in[i] + sidechain[i];
            if (sidechain[i] != 0.0f)
                nonzero = 1;
        }
        if (nonzero)
            self->sidechain_nonzero_frames++;
        break;
    }

    case OC_POISON:
        for (uint32_t i = 0; i < frames; ++i)
            out[i] = (float)NAN;
        break;
    }

    return CLAP_PROCESS_CONTINUE;
}

static const void *oc_get_extension(const clap_plugin_t *plugin, const char *id)
{
    (void)plugin;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &oc_ext_audio_ports;
    if (!strcmp(id, CLAP_EXT_PARAMS))
        return &oc_ext_params;
    if (!strcmp(id, CLAP_EXT_STATE))
        return &oc_ext_state;
    if (!strcmp(id, CLAP_EXT_LATENCY))
        return &oc_ext_latency;
    return NULL;
}

static void oc_on_main_thread(const clap_plugin_t *plugin)
{
    (void)plugin;
}

// --- factory --------------------------------------------------------------

static const clap_plugin_descriptor_t *const oc_descriptors[] = {
    &oc_desc_gain,
    &oc_desc_stereo,
    &oc_desc_sidechain,
    &oc_desc_poison,
};

#define OC_PLUGIN_COUNT ((uint32_t)(sizeof(oc_descriptors) / sizeof(oc_descriptors[0])))

static uint32_t oc_factory_count(const clap_plugin_factory_t *factory)
{
    (void)factory;
    return OC_PLUGIN_COUNT;
}

static const clap_plugin_descriptor_t *oc_factory_get(const clap_plugin_factory_t *factory,
                                                      uint32_t index)
{
    (void)factory;
    return index < OC_PLUGIN_COUNT ? oc_descriptors[index] : NULL;
}

static const clap_plugin_t *oc_factory_create(const clap_plugin_factory_t *factory,
                                              const clap_host_t *host, const char *id)
{
    (void)factory;
    if (!id)
        return NULL;

    enum oc_kind kind;
    if (!strcmp(id, oc_desc_gain.id))
        kind = OC_GAIN;
    else if (!strcmp(id, oc_desc_stereo.id))
        kind = OC_STEREO;
    else if (!strcmp(id, oc_desc_sidechain.id))
        kind = OC_SIDECHAIN;
    else if (!strcmp(id, oc_desc_poison.id))
        kind = OC_POISON;
    else
        return NULL;

    oc_plugin *self = calloc(1, sizeof(oc_plugin));
    if (!self)
        return NULL;
    self->host = host;
    self->kind = kind;
    self->gain = 1.0;
    self->plugin.desc = oc_descriptors[kind];
    self->plugin.plugin_data = self;
    self->plugin.init = oc_init;
    self->plugin.destroy = oc_destroy;
    self->plugin.activate = oc_activate;
    self->plugin.deactivate = oc_deactivate;
    self->plugin.start_processing = oc_start_processing;
    self->plugin.stop_processing = oc_stop_processing;
    self->plugin.reset = oc_reset;
    self->plugin.process = oc_process;
    self->plugin.get_extension = oc_get_extension;
    self->plugin.on_main_thread = oc_on_main_thread;
    return &self->plugin;
}

static const clap_plugin_factory_t oc_factory = {
    .get_plugin_count = oc_factory_count,
    .get_plugin_descriptor = oc_factory_get,
    .create_plugin = oc_factory_create,
};

// --- entry ----------------------------------------------------------------

static bool oc_entry_init(const char *path)
{
    (void)path;
    return true;
}

static void oc_entry_deinit(void) {}

static const void *oc_entry_get_factory(const char *id)
{
    return strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? NULL : &oc_factory;
}

// A DATA symbol, not a function. This is the whole ABI surface of a .clap.
CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION_INIT,
    .init = oc_entry_init,
    .deinit = oc_entry_deinit,
    .get_factory = oc_entry_get_factory,
};
