// mcpack-sync : HTTP 客户端（WinHTTP，支持代理与重试）
#pragma once

#include <map>
#include <string>
#include <vector>

#include "mcpsync/result.hpp"

namespace mcpsync {

struct HttpRequest {
    std::string                        method{"GET"};
    std::string                        url;
    std::map<std::string, std::string> headers;
    std::string                        body;
    int                                timeoutMs{30000};
    std::string                        proxy;      // http://host:port
    bool                               insecure{false};
};

struct HttpResponse {
    int                                                    status{0};
    std::string                                            body;
    std::map<std::string, std::string>                     headers;  // key 小写
    std::string header(std::string const& name) const;
};

Result<HttpResponse> httpRequest(HttpRequest const& req);

// 带重试的 GET（仅对 5xx / 网络错误重试）
Result<HttpResponse> httpGetJson(std::string const& url, std::map<std::string, std::string> const& headers, int timeoutMs, std::string const& proxy, int maxRetries);

std::string urlEncode(std::string_view s);
std::string base64Encode(std::string_view s);

} // namespace mcpsync
