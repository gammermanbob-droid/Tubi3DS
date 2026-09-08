#pragma once
#include <3ds.h>
#include <string>
struct Response { u32 status=0; Result error=0; std::string body; bool ok() const { return status==200 && R_SUCCEEDED(error); } };
Response get(const std::string& url,const std::string& token="");
