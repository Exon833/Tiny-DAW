// A benchmark: how long Exbit takes per buffer to play a plugin, against a bare reference that calls the same plugin
// directly (tests/plugin_speed/direct_calls.h), so what Exbit adds can be seen, the Wine bridge's above all.
//   Linux:   Surge XT 1.3.4's Linux VST3, in process (a sanity baseline: only the mixer is between);
//   Wine:    its Windows VST3 bridged through exbit-wine-host.exe, against plugin-speed-reference.exe (MinGW)
//            calling it in a Wine program of its own, in the test prefix ~/.local/share/exbit-test-wine/speed
//            (a copy of the `base` one, made when missing);
//   Windows: its Windows VST3 as Exbit plays it there (in exbit-plugin-host.exe, and in process), against
//            plugin-speed-reference.exe calling it directly (make tests-win).
// The plugins: ~/.local/share/tinydaw-test-plugins/{linux,win64}, or on Windows its VST3 folder. Each case (one
// instance; a chain of four on one track: the synth and three Surge XT Effects; eight tracks of one each, on the
// worker pool as the engine does) plays a sustained chord at 64, 256 and 1024 frames, 48 kHz, at real-time pace,
// REPEATS runs of each side in turn. Reported (build/plugin-speed/report.md, and printed): the median and 99th
// percentile per buffer, the bridge's overhead (Exbit minus the reference) and the buffers over their deadline.
// Checked, for Windows plugins: through a plugin host, at 256 and 1024 frames, the overhead at most each crossing's
// floor (tests/plugin_speed/floor_round_trip.h, measured first) plus CROSSING_ABOVE_FLOOR per crossing, plus the
// mixer's own overhead (what the in-process target measured for that case and size), plus PLUGIN_SLOWER_SHARE of the
// reference (the plugin's own code runs a little slower in a second process: fewer branches predicted, more TLB
// misses, with the same instructions); and no buffer of one instance over its deadline at 64 frames. A side whose plugins, Wine or MinGW are missing is skipped.
// EXBIT_SPEED_TARGET=<text> and EXBIT_SPEED_CASE=<text> measure only the targets or cases whose name holds it.
#include <chrono>
#include <cstdlib>
#include <iterator>
#include <map>
#include <thread>

#include "audio/engine.h"
#include "audio/realtime_priority.h"
#include "audio/workers.h"
#include "check.h"
#include "core/util.h"
#include "model/model.h"
#include "platform/platform.h"
#include "plugin_speed/benchmark_audio_thread.h"
#include "plugin_speed/buffer_stats.h"
#include "plugin_speed/direct_calls.h"
#include "plugin_speed/floor_round_trip.h"
#include "plugin_speed/mixer_calls.h"
#include "plugin_speed/reference_program.h"
#include "plugin_speed/speed_settings.h"
#include "plugins/library.h"
#include "plugins/wine.h"

using namespace plugin_speed;

namespace {

const std::string REPORT_PATH = "build/plugin-speed/report.md";
const double CROSSING_ABOVE_FLOOR = 5e-6;  // seconds a crossing may cost above the floor
const double PLUGIN_SLOWER_SHARE = 0.10;   // of the reference's median: the plugin computing slower in its host
const double MIXER_SHARE_FALLBACK = 0.05;  // of the reference's median: the mixer's allowance if not measured

// where the numbers are measured: the plugins as Exbit loads them, and how the bare reference calls them
struct SpeedTarget {
    std::string name;
    std::string instrument, effect;
    std::vector<std::string> referenceCommand;         // what runs the reference program; empty: in process
    std::string referenceInstrument, referenceEffect;  // the plugins' paths as the reference program names them
    bool windowsPlugin = false;                        // the bridge's limits are checked
    bool inProcess = false;                            // (Windows) without exbit-plugin-host.exe
};

std::string report;
double floorSeconds = 0;                             // one round trip's floor, measured first
std::map<std::string, double> mixerOverheadSeconds;  // ("case frames") what the in-process target measured

// (by hand) EXBIT_SPEED_TARGET=Wine, EXBIT_SPEED_CASE=eight…: only the targets or cases whose name holds it
bool wanted(const char* variable, const std::string& name) {
    const char* only = environment::readProgramVariable(variable);
    return !only || name.find(only) != std::string::npos;
}

// microseconds, as the report shows them
std::string micro(double seconds) {
    char text[32];
    snprintf(text, sizeof text, "%.1f", seconds * 1e6);
    return text;
}

// a table row of one case at one size, printed and added to the report; checks the limits for Windows plugins
void reportAndCheck(const SpeedTarget& target, const std::string& speedCase, int frames, const BufferStats& exbit,
                    const BufferStats& reference) {
    const double overhead = exbit.medianSeconds - reference.medianSeconds;
    const double overheadShare = reference.medianSeconds > 0 ? overhead / reference.medianSeconds : 0;
    char share[32];
    snprintf(share, sizeof share, "%+.1f%%", overheadShare * 100);
    std::string row = "| " + speedCase + " | " + std::to_string(frames) + " | " + micro((double)frames / SAMPLE_RATE) +
                      " | " + micro(exbit.medianSeconds) + " | " + micro(reference.medianSeconds) + " | " +
                      micro(overhead) + " | " + share + " | " + micro(exbit.percentile99Seconds) + " | " +
                      micro(reference.percentile99Seconds) + " | " + std::to_string(exbit.overDeadline) + "/" +
                      std::to_string(exbit.buffers) + " | " + std::to_string(reference.overDeadline) + "/" +
                      std::to_string(reference.buffers) + " |\n";
    report += row;
    printf("   %s", row.c_str());
    const std::string key = speedCase + " " + std::to_string(frames);
    const bool bridged = target.windowsPlugin && !target.inProcess;
    if (!bridged) mixerOverheadSeconds[key] = std::max(0.0, overhead);
    if (!target.windowsPlugin) return;
    if (bridged && frames >= 256) {
        const int crossings = speedCase == "one" ? 1 : speedCase == "chain" ? 4 : 8;  // (per buffer)
        const auto mixer = mixerOverheadSeconds.find(key);
        const double budget = crossings * (floorSeconds + CROSSING_ABOVE_FLOOR) +
                              PLUGIN_SLOWER_SHARE * reference.medianSeconds +
                              (mixer != mixerOverheadSeconds.end() ? mixer->second
                                                                   : MIXER_SHARE_FALLBACK * reference.medianSeconds);
        if (overhead > budget)
            printf("   FAILS: %s, %s, %d frames: Exbit adds %s µs to the reference's time (budget %s µs: %d "
                   "crossing%s of %s µs floor + %s µs, the plugin %.0f%% slower, and the mixer's own)\n",
                   target.name.c_str(), speedCase.c_str(), frames, micro(overhead).c_str(), micro(budget).c_str(),
                   crossings, crossings == 1 ? "" : "s", micro(floorSeconds).c_str(),
                   micro(CROSSING_ABOVE_FLOOR).c_str(), PLUGIN_SLOWER_SHARE * 100);
        CHECK(overhead <= budget);
    }
    if (frames == 64 && speedCase == "one") {
        if (exbit.overDeadline > 0)
            printf("   FAILS: %s, one instance, 64 frames: %d of %d buffers over their deadline\n", target.name.c_str(),
                   exbit.overDeadline, exbit.buffers);
        CHECK(exbit.overDeadline == 0);
    }
}

// waits (up to a minute) until every plugin host's audio thread is real time, as the engine's own threads are: rtkit
// refuses what goes past its 25 requests per 20 s and Exbit asks again a window later (audio/realtime_promotion.h),
// and a host left at normal priority loses its core to the real-time workers, which would be measured instead
void waitForRealtimeHosts() {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (bool said = false;; said = true) {
        const std::string status = realtime::statusText();
        const size_t line = status.find("Plugin hosts: ");
        int granted = 0, hosts = 0;
        if (line == std::string::npos ||
            (sscanf(status.c_str() + line, "Plugin hosts: %d of %d", &granted, &hosts) == 2 && granted == hosts))
            return;
        if (std::chrono::steady_clock::now() > until) {
            printf("   (not every plugin host got real time: %d of %d)\n", granted, hosts);
            return;
        }
        if (!said) printf("   (waiting for rtkit to grant every plugin host real time: %d of %d so far)\n", granted, hosts);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// every case of the target: Exbit's side and the reference's, REPEATS runs each in turn, reported and checked
void measureTarget(const SpeedTarget& target) {
    printf("== %s\n", target.name.c_str());
    if (target.inProcess) unsetenv("EXBIT_PLUGIN_HOST");  // (Windows: Plugin::load then hosts it in process)
    report += "\n## " + target.name + "\n\n| case | frames | deadline µs | Exbit median µs | reference median µs | "
              "overhead µs | overhead | Exbit p99 µs | reference p99 µs | Exbit over deadline | reference over deadline |\n"
              "|---|---|---|---|---|---|---|---|---|---|---|\n";
    for (const std::string speedCase : {"one", "chain", "eight"}) {
        if (!wanted("SPEED_CASE", speedCase)) continue;
        std::string error;
        std::vector<std::vector<std::vector<double>>> exbitRuns(std::size(FRAME_SIZES)), referenceRuns(std::size(FRAME_SIZES));
        for (int run = 0; run < REPEATS && error.empty(); run++) {
            std::vector<std::vector<double>> exbit, reference;
            printf("   %s, run %d of %d: Exbit, then the reference\n", speedCase.c_str(), run + 1, REPEATS);
            {  // (the project's plugins gone before the reference loads its own: never two sets at once)
                Project project;
                if (!buildSpeedProject(project, speedCase, target.instrument, target.effect, error)) {
                    error = "a plugin didn't load in Exbit: " + error;
                    break;
                }
                waitForRealtimeHosts();
                exbit = timeMixerCalls(project);
            }
            if (target.referenceCommand.empty()) {
                runOnAudioThread([&](workers::Pool& pool) {
                    reference = timeDirectCalls(speedCase, target.instrument, target.effect, pool, error);
                });
            } else {
                auto command = target.referenceCommand;
                command.insert(command.end(), {speedCase, target.referenceInstrument, target.referenceEffect});
                reference = runReferenceProgram(command, error);
            }
            for (size_t size = 0; size < reference.size(); size++) {
                exbitRuns[size].push_back(exbit[size]);
                referenceRuns[size].push_back(reference[size]);
            }
        }
        if (!error.empty()) {
            printf("   %s: failed: %s\n", speedCase.c_str(), error.c_str());
            CHECK(false);
            continue;
        }
        for (size_t size = 0; size < std::size(FRAME_SIZES); size++)
            reportAndCheck(target, speedCase, FRAME_SIZES[size], summarizeRuns(exbitRuns[size], FRAME_SIZES[size]),
                           summarizeRuns(referenceRuns[size], FRAME_SIZES[size]));
    }
}

// the targets this machine has, each skipped one said why
std::vector<SpeedTarget> availableTargets() {
    std::vector<SpeedTarget> targets;
    const std::string reference = util::joinPath(plat::executableDir(), "../plugin-speed-reference.exe");
    const bool referenceBuilt = util::fileExists(reference);
#ifdef _WIN32
    const char* common = getenv("CommonProgramFiles");
    const std::string vst3 = util::joinPath(common ? common : "C:/Program Files/Common Files", "VST3");
    SpeedTarget windows;
    windows.instrument = windows.referenceInstrument = plug::loadableFile(util::joinPath(vst3, "Surge XT.vst3"));
    windows.effect = windows.referenceEffect = plug::loadableFile(util::joinPath(vst3, "Surge XT Effects.vst3"));
    windows.referenceCommand = {reference};
    windows.windowsPlugin = true;
    if (!util::fileExists(windows.instrument) || !util::fileExists(windows.effect) || util::isDir(windows.instrument)) {
        printf("== Windows: skipped, Surge XT's VST3s aren't in %s\n", vst3.c_str());
    } else if (!referenceBuilt) {
        printf("== Windows: skipped, %s isn't built\n", reference.c_str());
    } else {
        std::string why;
        windows.name = "Windows (in process)";  // (first: the mixer's own overhead, which the host's budget holds)
        windows.inProcess = true;
        targets.push_back(windows);
        windows.name = "Windows (exbit-plugin-host.exe)";
        windows.inProcess = false;
        if (wine::available(why)) targets.push_back(windows);
        else printf("== %s: skipped, %s\n", windows.name.c_str(), why.c_str());
    }
#else
    const std::string plugins = util::joinPath(plat::homeDir(), ".local/share/tinydaw-test-plugins");
    SpeedTarget linux;
    linux.name = "Linux (in process)";
    linux.instrument = plug::loadableFile(plugins + "/linux/Surge XT.vst3");
    linux.effect = plug::loadableFile(plugins + "/linux/Surge XT Effects.vst3");
    if (util::fileExists(linux.instrument) && util::fileExists(linux.effect) && !util::isDir(linux.instrument))
        targets.push_back(linux);
    else
        printf("== %s: skipped, Surge XT's Linux VST3s aren't in %s/linux\n", linux.name.c_str(), plugins.c_str());
    SpeedTarget wine;
    wine.name = "Wine (exbit-wine-host.exe)";
    wine.instrument = plug::loadableFile(plugins + "/win64/Surge XT.vst3");
    wine.effect = plug::loadableFile(plugins + "/win64/Surge XT Effects.vst3");
    wine.referenceInstrument = "Z:" + wine.instrument;
    wine.referenceEffect = "Z:" + wine.effect;
    wine.windowsPlugin = true;
    const std::string testPrefixes = util::joinPath(plat::homeDir(), ".local/share/exbit-test-wine");
    const std::string prefix = testPrefixes + "/speed";
    std::string why;
    if (!util::fileExists(wine.instrument) || !util::fileExists(wine.effect) || util::isDir(wine.instrument)) {
        printf("== %s: skipped, Surge XT's Windows VST3s aren't in %s/win64\n", wine.name.c_str(), plugins.c_str());
    } else if (!wine::available(why)) {
        printf("== %s: skipped, %s\n", wine.name.c_str(), why.c_str());
    } else if (!referenceBuilt) {
        printf("== %s: skipped, %s isn't built (it needs MinGW: x86_64-w64-mingw32-g++)\n", wine.name.c_str(),
               reference.c_str());
    } else if (!wine::isPrefix(prefix) && (!wine::isPrefix(testPrefixes + "/base") ||
                                           system(("cp -a '" + testPrefixes + "/base' '" + prefix + "'").c_str()) != 0)) {
        printf("== %s: skipped, no test prefix %s (made from %s/base)\n", wine.name.c_str(), prefix.c_str(),
               testPrefixes.c_str());
    } else {
        wine::setPrefixes({prefix});
        // (Wine's settings as Exbit starts its hosts with: plugins/wine.cpp, spawnHost)
        wine.referenceCommand = {"env", "WINEPREFIX=" + prefix, "WINEDEBUG=-all",
                                 "WINEDLLOVERRIDES=winedbg.exe=d;dxgi,d3d11,d3d10core=b", wine::winePathFor(prefix),
                                 reference};
        targets.push_back(wine);
    }
#endif
    return targets;
}

}  // namespace

int main(int argc, char** argv) {
    if (answerFloorRoundTrips(argc, argv)) return 0;
    setvbuf(stdout, nullptr, _IOLBF, 0);  // (each row as it is measured, also into a log)
    Project host;
    audio::Engine engine(host);  // (installs what MIDI clips play through)
    char header[512];
    snprintf(header, sizeof header,
             "# Plugin speed\n\nSurge XT 1.3.4 playing an 8-note chord, %d Hz, at real-time pace; %d runs of %.1f s "
             "per side, taken in turn. Median: of the runs' medians. Overhead: Exbit's median minus the reference's "
             "(the plugin called directly). %d workers in the pool.\n",
             SAMPLE_RATE, REPEATS, SECONDS_PER_RUN, workers::workersFor(0));
    report = header;
    floorSeconds = floorRoundTrip();
    report += "\nA crossing's floor (a bare round trip between two processes, 64 frames): " + micro(floorSeconds) + " µs.\n";
    printf("a crossing's floor: %s µs\n", micro(floorSeconds).c_str());
    CHECK(floorSeconds > 0);
    for (const auto& target : availableTargets())
        if (wanted("SPEED_TARGET", target.name)) measureTarget(target);
    util::makeDirs(util::dirName(REPORT_PATH));
    CHECK(util::writeFile(REPORT_PATH, report));
    printf("report: %s\n", REPORT_PATH.c_str());
#ifndef _WIN32
    const std::string prefix = util::joinPath(plat::homeDir(), ".local/share/exbit-test-wine/speed");
    if (wine::isPrefix(prefix))  // (no Wine left running in the test prefix)
        system(("env WINEPREFIX='" + prefix + "' '" + util::joinPath(util::dirName(wine::winePath()), "wineserver") +
                "' -k 2>/dev/null").c_str());
#endif
    TEST_MAIN_END();
}
