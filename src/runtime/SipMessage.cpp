// SPDX-License-Identifier: MIT

#include "hmr/runtime/SipMessage.hpp"

#include <cctype>

namespace hmr::runtime {

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

}  // namespace hmr::runtime

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

}  // namespace

const HmrSipMsg::Header* HmrSipMsg::find(std::string_view name) const {
    for (const Header& h : headers)
        if (hmr::runtime::iequals(h.name, name)) return &h;
    return nullptr;
}

std::string_view HmrSipMsg::getHeader(std::string_view name) const {
    const Header* h = find(name);
    return h ? std::string_view{h->value} : std::string_view{};
}

bool HmrSipMsg::setHeader(std::string_view name, std::string_view value) {
    for (Header& h : headers) {
        if (hmr::runtime::iequals(h.name, name)) {
            h.value.assign(value);
            return true;
        }
    }
    addHeader(name, value);
    return false;
}

void HmrSipMsg::addHeader(std::string_view name, std::string_view value) {
    headers.push_back({std::string(name), std::string(value)});
}

bool HmrSipMsg::deleteHeader(std::string_view name) {
    bool removed = false;
    for (std::size_t i = 0; i < headers.size();) {
        if (hmr::runtime::iequals(headers[i].name, name)) {
            headers.erase(headers.begin() + static_cast<std::ptrdiff_t>(i));
            removed = true;
        } else {
            ++i;
        }
    }
    return removed;
}

HmrSipMsg HmrSipMsg::parse(std::string_view raw) {
    HmrSipMsg msg;
    std::size_t pos = 0;
    bool firstLine = true;

    auto nextLine = [&](std::string_view& out) -> bool {
        if (pos > raw.size()) return false;
        std::size_t nl = raw.find('\n', pos);
        if (nl == std::string_view::npos) {
            out = raw.substr(pos);
            pos = raw.size() + 1;
        } else {
            out = raw.substr(pos, nl - pos);
            pos = nl + 1;
        }
        return true;
    };

    std::string_view line;
    while (nextLine(line)) {
        std::string_view t = trim(line);
        if (firstLine) {
            firstLine = false;
            // Status line: "SIP/2.0 200 OK"; request line: "INVITE sip:.. SIP/2.0"
            if (t.starts_with("SIP/")) {
                msg.isRequest = false;
                std::size_t sp1 = t.find(' ');
                std::size_t sp2 =
                    sp1 == std::string_view::npos ? sp1 : t.find(' ', sp1 + 1);
                if (sp1 != std::string_view::npos) {
                    std::string_view code = t.substr(
                        sp1 + 1, (sp2 == std::string_view::npos ? t.size()
                                                                : sp2) -
                                     sp1 - 1);
                    msg.statusCode = 0;
                    for (char c : code)
                        if (c >= '0' && c <= '9')
                            msg.statusCode =
                                msg.statusCode * 10 +
                                static_cast<std::uint32_t>(c - '0');
                    if (sp2 != std::string_view::npos)
                        msg.reasonPhrase = std::string(trim(t.substr(sp2 + 1)));
                }
            } else {
                msg.isRequest = true;
                std::size_t sp1 = t.find(' ');
                if (sp1 != std::string_view::npos) {
                    msg.method = std::string(t.substr(0, sp1));
                    std::size_t sp2 = t.find(' ', sp1 + 1);
                    msg.requestUri = std::string(trim(t.substr(
                        sp1 + 1, (sp2 == std::string_view::npos ? t.size()
                                                                : sp2) -
                                     sp1 - 1)));
                }
            }
            continue;
        }
        if (t.empty()) break;  // end of headers (start of body)

        std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) continue;  // malformed; skip
        std::string_view name = trim(line.substr(0, colon));
        std::string_view value = trim(line.substr(colon + 1));
        msg.headers.push_back({std::string(name), std::string(value)});
    }
    return msg;
}

std::string HmrSipMsg::toString() const {
    std::string out;
    if (isRequest) {
        out += method;
        out.push_back(' ');
        out += requestUri;
        out += " SIP/2.0\r\n";
    } else {
        out += "SIP/2.0 ";
        out += std::to_string(statusCode);
        out.push_back(' ');
        out += reasonPhrase;
        out += "\r\n";
    }
    for (const Header& h : headers) {
        out += h.name;
        out += ": ";
        out += h.value;
        out += "\r\n";
    }
    out += "\r\n";
    return out;
}
