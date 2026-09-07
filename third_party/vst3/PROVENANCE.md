# VST 3 plug-in interfaces

Upstream: <https://github.com/steinbergmedia/vst3_pluginterfaces>
Commit: `4f547e8e102b47de4a8b8aaf343c73b700786372`
Licence: MIT (`LICENSE.txt`)

Kept: everything except `test/` and the repository's own git metadata. This is
the interface definition half of the VST 3 SDK — the part a *host* needs. The
rest of the SDK (`vst3sdk` proper, with its `base`, `public.sdk` and `vstgui`
submodules) exists to help somebody *write a plugin*, and a host that only ever
consumes the published vtables does not need any of it.

Four `.cpp` files come with the headers and are compiled into `openchat_vst3`:
`funknown.cpp` and `coreiids.cpp` define the interface IIDs that
`DECLARE_CLASS_IID` only declares, and `conststringtable.cpp` / `ustring.cpp`
back the string helpers those pull in. Without them every `FUnknown::iid`
reference is an undefined symbol at link time.

Note on the name: Steinberg relicensed the SDK from the GPLv3/proprietary dual
licence to plain MIT with VST 3.8 (October 2025), which is what makes vendoring
this possible at all. The *trademark* is unaffected and still forbids "VST" in
a product or feature name — which is why this subsystem is called voice
effects, and why no type in `src/effects/` is named after it.

Updating: replace wholesale from upstream, re-delete `test/`, and re-check that
the four `.cpp` files above are still the complete set (a new one appearing
means a new undefined symbol at link time, not a silent miscompile).
