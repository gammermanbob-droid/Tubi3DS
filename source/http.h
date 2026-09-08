#pragma once
#include <3ds.h>
#include <string>
// finalUrl: the URL this response actually came from, after following any
// redirect chain internally (fetch() in http.cpp follows 30x redirects on
// its own and the caller never sees the intermediate hops). Defaults to
// whatever was requested, updated to the last hop's URL when a redirect was
// followed. This matters whenever a caller needs to resolve a relative or
// absolute-path reference found INSIDE the response body (e.g. an HLS
// master playlist's variant URIs) -- resolving against the originally
// requested URL instead of finalUrl silently reconstructs URLs against the
// wrong host/session if the response was redirected, which is exactly what
// turned out to be happening for Tubi's live channel manifests (see
// Catalog::resolve()'s use of this field and its comment).
struct Response { u32 status=0; Result error=0; std::string body; std::string finalUrl; bool ok() const { return status==200 && R_SUCCEEDED(error); } };
Response get(const std::string& url,const std::string& token="");
