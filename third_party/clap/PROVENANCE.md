# CLAP

Upstream: <https://github.com/free-audio/clap>
Version: 1.2.6
Commit: `69a69252fdd6ac1d06e246d9a04c0a89d9607a17`
Licence: MIT (`LICENSE`)

Kept: `include/` only. The upstream repository also carries `src/`, `artwork/`,
`resource/` and its own CMake project; none of it is needed to host a plugin.
CLAP is a pure ABI description — there is no library to link and nothing to
compile, which is the whole reason the host side is 18 headers rather than an
SDK.

Updating: replace `include/` wholesale from a tagged upstream release and
update the version and commit above. Do not patch the headers in place; a
local change to an ABI header is a change to the contract every plugin was
compiled against.
