// Cross-validates the device's POSIX parser against the browser's derivation.
//
// The config page turns an IANA zone into a POSIX rule using Intl; the clock
// parses that rule with src/tz.cpp. If those two disagree the clock is silently
// hours off, and no test that only exercises one side would notice.
//
// Vectors come from tools/gen_tz_vectors.mjs, where the expected offsets are
// Intl's answers -- not ours -- so this checks us against the real tz database.
//
//   node tools/gen_tz_vectors.mjs > tests/tz_vectors.txt
//   make tzvec_test && ./tzvec_test

#include "../src/tz.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "tz_vectors.txt";
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("SKIP no %s -- run: node ../tools/gen_tz_vectors.mjs > %s\n", path, path);
        return 0;
    }

    char line[16384];
    int zones = 0, samples = 0, badZones = 0, badSamples = 0;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        char *zone = strtok(line, "\t");
        char *rule = strtok(nullptr, "\t");
        char *list = strtok(nullptr, "\t\n");
        if (!zone || !rule || !list) continue;

        zones++;
        TzInfo tz;
        if (!tzParse(rule, tz)) {
            printf("FAIL %-24s rule did not parse: %s\n", zone, rule);
            badZones++;
            continue;
        }

        int mismatches = 0;
        long firstEpoch = 0, firstWant = 0, firstGot = 0;

        for (char *tok = strtok(list, ","); tok; tok = strtok(nullptr, ",")) {
            char *colon = strchr(tok, ':');
            if (!colon) continue;
            *colon = 0;
            long epoch = atol(tok);
            long want = atol(colon + 1);
            long got = tzOffsetFor(tz, (int64_t)epoch);
            samples++;
            if (got != want) {
                if (!mismatches) { firstEpoch = epoch; firstWant = want; firstGot = got; }
                mismatches++;
                badSamples++;
            }
        }

        if (mismatches) {
            printf("FAIL %-24s %-38s %d/%d wrong (epoch %ld: want %+ld, got %+ld)\n",
                   zone, rule, mismatches, 0, firstEpoch, firstWant, firstGot);
            badZones++;
        } else {
            printf("ok   %-24s %s\n", zone, rule);
        }
    }
    fclose(f);

    printf("\n%d zones, %d sampled instants, %d zones wrong, %d instants wrong\n",
           zones, samples, badZones, badSamples);
    printf("%s\n", badZones ? "FAILED" : "all passed");
    return badZones != 0;
}
