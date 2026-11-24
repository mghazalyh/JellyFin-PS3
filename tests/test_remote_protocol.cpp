#include "../source/player/remote_protocol.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>

static bool parse(const std::string &s, JfRemoteCommand *out) {
    return jf_remote_parse(s.data(), s.size(), out);
}
static std::string valid(const char *action = "get_status") {
    return std::string("JFR1\nid=0123456789abcdef\nauth=") +
           std::string(64, 'a') +
           "\ngeneration=123\naction=" + action + "\n";
}
int main() {
    JfRemoteCommand c = {};
    const std::string ok = valid();
    assert(parse(ok, &c));
    assert(!strcmp(c.id, "0123456789abcdef"));
    assert(!strcmp(c.action, "get_status"));
    assert(c.generation == 123);
    assert(!c.has_value);
    assert(jf_remote_auth_matches(c.auth, std::string(64, 'a').c_str()));
    assert(!jf_remote_auth_matches(c.auth, std::string(64, 'b').c_str()));
    assert(!jf_remote_auth_matches(c.auth, "short"));
    assert(!jf_remote_auth_matches("", c.auth));
    assert(jf_remote_action_valid("favorite_previous"));
    assert(jf_remote_action_valid("play_previous"));
    assert(parse(valid("play_previous"), &c));
    assert(!strcmp(c.action, "play_previous") && !c.mode[0]);
    assert(!parse(valid("play_previous") + "mode=all\n", &c));
    assert(!jf_remote_action_valid("exec"));
    JfRemoteReplay replay = {};
    assert(jf_remote_record_id(&replay, "0123456789abcdef"));
    assert(!jf_remote_record_id(&replay, "0123456789abcdef"));
    assert(!jf_remote_record_id(&replay, "invalid"));
    // Rolling 16-entry history and deterministic oldest-entry eviction.
    for (unsigned int i = 0; i < 15; ++i) {
        char id[17];
        snprintf(id, sizeof id, "%016llx", (unsigned long long)(i + 1));
        assert(jf_remote_record_id(&replay, id));
        assert(!jf_remote_record_id(&replay, id));
    }
    assert(jf_remote_record_id(&replay, "0123456789abcdef") == false);
    assert(jf_remote_record_id(&replay, "0000000000000010"));
    assert(jf_remote_record_id(&replay, "0123456789abcdef"));

    std::string enabled = valid("set_audio_sync_beta") + "value=1\n";
    assert(parse(enabled, &c) && c.has_value && c.value == 1);
    assert(!parse(valid("set_audio_sync_beta"), &c));
    assert(!parse(valid("get_status") + "value=1\n", &c));
    assert(!parse(valid("get_status") + "action=get_status\n", &c));
    assert(!parse(valid("get_status") + "path=/dev_hdd0\n", &c));
    assert(!parse(valid("run_shell"), &c));
    assert(!parse(valid("stop_playback").substr(0, valid("stop_playback").size() - 1), &c));
    assert(!parse(std::string(512, 'a'), &c));
    assert(!parse("JFR2\n" + ok.substr(5), &c));
    assert(!parse("JFR1\n" + ok.substr(5).replace(0, 3, "iD="), &c));
    assert(!parse(valid("get_status") + "value=2\n", &c));
    {
        std::string x = ok; x[10] = '\0';
        assert(!parse(x, &c));
    }
    {
        std::string x = ok; x.replace(x.find("generation=123"), 14,
                                      "generation=4294967296");
        assert(!parse(x, &c));
    }
    {
        std::string x = ok; x.replace(x.find("generation=123"), 14,
                                      "generation=-1");
        assert(!parse(x, &c));
    }
    {
        std::string x = ok; x.replace(x.find("id=0123456789abcdef"), 19,
                                      "id=0123456789abcdeg");
        assert(!parse(x, &c));
    }
    {
        std::string x = ok; x.replace(x.find("auth=") + 5, 1, "A");
        assert(!parse(x, &c));
    }
    uint32_t result = 0;
    assert(jf_remote_decimal("4294967295", &result) && result == UINT32_MAX);
    assert(!jf_remote_decimal("4294967296", &result));
    assert(!jf_remote_decimal("1x", &result));
    assert(!jf_remote_decimal("", &result));
    // Deterministic malformed-input stress without dynamic allocations in parser.
    uint32_t seed = 0xdecafbadU;
    for (unsigned int i = 0; i < 100000; ++i) {
        std::string x = ok;
        seed = seed * 1664525U + 1013904223U;
        const size_t at = seed % x.size();
        x[at] = (char)(seed >> 24);
        JfRemoteCommand scratch = {};
        (void)parse(x, &scratch);
    }
    puts("PASS remote protocol strict parser, bounds, 100000 mutations");
    return 0;
}
