
#include <ethash/progpow.hpp>
#include "../../test/unittests/helpers.hpp"
#include <ethash/ethash.hpp>
#include <ethash/ethash-internal.hpp>
#include <climits>
#include <memory>
#include <cstdint>
#include <iostream>
#include <evhttp.h>

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h> /* strtoumax */
#include <stdbool.h>
#include <errno.h>

#define DEFAULT_PORT 8799

static bool str_to_uint16(const char *str, uint16_t *res);

int main(int argc, const char* argv[])
{
    static int epoch_number = 0;
    std::uint16_t SrvPort = DEFAULT_PORT;
    static ethash::epoch_context_full_ptr context_full{nullptr, nullptr};

    for (int i = 0; i < argc; ++i)
    {
        const std::string arg{argv[i]};

        if (arg == "-p" && i + 1 < argc) {
            if (!str_to_uint16(argv[++i], &SrvPort)) {
                fprintf(stderr, "conversion error\n");
                exit(2);
            }
        }

        if (arg == "-e" && i + 1 < argc)
            epoch_number = atoi(argv[++i]);
    }

    // Build context
    if (!context_full || context_full->epoch_number != epoch_number) {
        std::cout << "Building context for epoch: " << epoch_number << std::endl;
        context_full = ethash::create_epoch_context_full(epoch_number);

    }

    if (!event_init())
    {
        std::cerr << "Failed to init libevent." << std::endl;
        return -1;
    }

    // Setup server
    char const SrvAddress[] = "127.0.0.1";
    std::unique_ptr<evhttp, decltype(&evhttp_free)> Server(evhttp_start(SrvAddress, SrvPort), &evhttp_free);
    if (!Server)
    {
        std::cerr << "Failed to init http server." << std::endl;
        return -1;
    }

    std::cout << "Server started!" << std::endl;
    std::cout << "Listening on port: " << SrvPort << std::endl;

    void (*OnReq)(evhttp_request *req, void *) = [] (evhttp_request *req, void *)
    {
        auto *OutBuf = evhttp_request_get_output_buffer(req);

        if (!OutBuf)
            return;

        // Construct the query struct
        struct evkeyvalq headers{};
        const struct evhttp_uri *uri = evhttp_request_get_evhttp_uri(req);
        const char * query = evhttp_uri_get_query(uri);
        evhttp_parse_query_str(query, &headers);

        // Get the headers from the query struct
        const char* header_hash_str = evhttp_find_header(&headers, "header_hash");
        const char* mix_hash_str = evhttp_find_header(&headers, "mix_hash");
        const char* nonce_str = evhttp_find_header(&headers, "nonce");
        const char* height_str = evhttp_find_header(&headers, "height");
        const char* share_boundary_str = evhttp_find_header(&headers, "share_boundary");
        const char* block_boundary_str = evhttp_find_header(&headers, "block_boundary");

        if (!header_hash_str || !mix_hash_str || !nonce_str || !height_str || !share_boundary_str || !block_boundary_str) {
            std::string error = "";

            if (!header_hash_str)
                error = "Invalid header_hash";
            else if (!mix_hash_str)
                error = "Invalid mix_hash";
            else if (!nonce_str)
                error = "Invalid nonce";
            else if (!height_str)
                error = "Invalid height";
            else if (!share_boundary_str)
                error = "Invalid share_boundary";
            else if (!block_boundary_str)
                error = "Invalid block_boundary";

            evhttp_send_reply(req, HTTP_BADREQUEST, error.c_str(), OutBuf);
        } else {
            auto header_hash = to_hash256(header_hash_str);
            auto mix_hash = to_hash256(mix_hash_str);
            auto share_boundary = to_hash256(share_boundary_str);
            auto block_boundary = to_hash256(block_boundary_str);

            // Convert nonce from string
            uint64_t nNonce;
            errno = 0;
            char *endp = nullptr;
            errno = 0; // strtoull will not set errno if valid
            unsigned long long int n = strtoull(nonce_str, &endp, 16);
            nNonce = (uint64_t) n;

            // Convert height from string
            uint32_t nHeight;
            errno = 0;
            endp = nullptr;
            errno = 0; // strtoul will not set errno if valid
            unsigned long int nH = strtoul(height_str, &endp, 10);
            nHeight = (uint32_t) nH;

            // Check epoch number and context
            epoch_number = (int) nHeight / ETHASH_EPOCH_LENGTH;
            if (!context_full || context_full->epoch_number != epoch_number) {
                std::cout << "Building new context for epoch: " << epoch_number << std::endl;
                context_full = ethash::create_epoch_context_full(epoch_number);
            }

            const auto result = progpow::hash(*context_full, (int) nHeight, header_hash, nNonce);
            std::string share_met = "false";
            std::string block_met = "false";
            std::string mix_match = "false";
            if (result.mix_hash == mix_hash) {
                mix_match = "true";
            }

            if (ethash::is_less_or_equal(result.final_hash, share_boundary)) {
                share_met = "true";
            }

            if (ethash::is_less_or_equal(result.final_hash, block_boundary)) {
                block_met = "true";
            }

            evbuffer_add_printf(OutBuf, "{\"%s\":%s, \"%s\":%s, \"%s\":%s, \"%s\":\"%s\"}", "result", mix_match.c_str(),
                                "share", share_met.c_str(), "block", block_met.c_str(), "digest",
                                to_hex(result.final_hash).c_str());
            evhttp_send_reply(req, HTTP_OK, "", OutBuf);
        }
    };
    evhttp_set_gencb(Server.get(), OnReq, nullptr);
    if (event_dispatch() == -1)
    {
        std::cerr << "Failed to run message loop." << std::endl;
        return -1;
    }
    return 0;
}

static bool str_to_uint16(const char *str, uint16_t *res)
{
    char *end;
    errno = 0;
    intmax_t val = strtoimax(str, &end, 10);
    if (errno == ERANGE || val < 0 || val > UINT16_MAX || end == str || *end != '\0')
        return false;
    *res = (uint16_t) val;
    return true;
}