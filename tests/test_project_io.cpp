// Projects: save → load round trips, the audio folder (shared files, never overwritten), the JSON
// layout of the Python program, cables / automation / racks / frozen tracks / timing, old and
// broken project files.
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <ctime>
#include <string>
#include <vector>

#include "audio/sound_file.h"
#include "check.h"
#include "core/file_birth_time.h"
#include "core/json.h"
#include "core/util.h"
#include "model/project_io.h"

using namespace project_io;

static const std::string kDir = "build/test-tmp/project";
static const int SR = 48000;

static void clearDir(const std::string& dir) {
    if (!util::isDir(dir)) return;
    for (auto& e : util::listDir(dir)) {
        std::string p = util::joinPath(dir, e.name);
        if (e.isDir) {
            clearDir(p);
            rmdir(p.c_str());
        } else {
            util::removeFile(p);
        }
    }
}

static size_t wavCount(const std::string& folder) {
    size_t n = 0;
    for (auto& e : util::listDir(folder))
        if (!e.isDir && util::extension(e.name) == ".wav") n++;
    return n;
}

static std::shared_ptr<AudioBuffer> tone(int64_t frames, double f, double amp) {
    auto b = std::make_shared<AudioBuffer>();
    b->samples.resize(frames * 2);
    for (int64_t i = 0; i < frames; i++) {
        b->samples[2 * i] = (float)(amp * std::sin(i * f));
        b->samples[2 * i + 1] = (float)(amp * std::cos(i * f * 1.01));
    }
    return b;
}

static SlotPtr slot(const std::string& path, const std::string& name, const std::string& kind, const std::string& state) {
    auto s = std::make_shared<PluginSlot>();
    s->path = path;
    s->name = name;
    s->kind = kind;
    s->state = state;
    return s;
}

struct Built {
    Project p;
    TrackPtr A, B, M, R, C;
    SlotPtr inst, fx1, fx2, r1, r2, mfx;
    ClipPtr c1, c2, c3, c4, mc;
};

static void build(Built& b) {
    Project& p = b.p;
    b.A = p.addTrack(Kind::Audio);
    b.B = p.addTrack(Kind::Audio);
    b.M = p.addTrack(Kind::Midi);
    b.R = p.addTrack(Kind::Midi);
    b.C = p.addTrack(Kind::Audio);
    b.A->color = "#ff0000";
    b.A->tone = "dark";
    b.A->volumeDb = -3.5;
    b.A->muted = true;
    b.A->height = 100;
    b.A->output = 2;
    b.B->soloed = true;
    GroupPtr g = p.groupTracks({b.B, b.M});
    g->color = "#00ff00";
    g->tone = "light";
    g->folded = true;
    g->muted = true;

    b.M->needsInstrument = false;
    b.inst = slot("/usr/lib/vst3/Synth.vst3", "Synth", "instrument", std::string("\x00\x01state\xff\x80", 9));
    b.inst->pluginName = "Synth A";
    b.M->instrument = b.inst;
    b.fx1 = slot("/home/me/.vst3/Filter.vst3", "Filter", "effect", "abc");
    b.fx1->placed = true;
    b.fx1->atX = 10;
    b.fx1->atY = 20;
    b.fx2 = slot("/home/me/.vst/Comp.so", "Comp", "effect", "");
    b.fx2->bypass = true;
    b.M->effects = std::vector<SlotPtr>{b.fx1, b.fx2};
    b.M->cables = std::vector<Cable>{{End::in(), End::of(b.fx1.get())},
                                     {End::of(b.fx1.get()), End::of(b.fx2.get())},
                                     {End::source(b.A.get()), End::sidechain(b.fx2.get())},
                                     {End::of(b.fx2.get()), End::out()}};
    auto lane1 = std::make_shared<Automation>();
    lane1->slot = b.fx1;
    lane1->param = "Cutoff";
    lane1->height = 80;
    lane1->setPoints({{0, 0.5}, {1.5, 1.0}, {3, 0.25}});
    auto lane2 = std::make_shared<Automation>();
    lane2->slot = b.inst;
    lane2->param = "Volume";
    lane2->setPoints({{0.5, 0.75}});
    b.M->automation = std::vector<AutomationPtr>{lane1, lane2};
    b.M->showAutomation = true;

    b.R->needsInstrument = false;
    b.R->usesRack = true;
    b.r1 = slot("/usr/lib/vst3/Piano.vst3", "Piano", "instrument", "p1");
    b.r1->volumeDb = -6;
    b.r1->placed = true;
    b.r1->atX = 300;
    b.r1->atY = -40;
    b.r2 = slot("/usr/lib/vst3/Pad.vst3", "Pad", "instrument", "p2");
    b.R->rack = std::vector<SlotPtr>{b.r1, b.r2};
    b.R->rackCables = std::vector<Cable>{{End::in(), End::of(b.r1.get())}, {End::of(b.r1.get()), End::out()},
                                         {End::in(), End::of(b.r2.get())}};
    auto lane3 = std::make_shared<Automation>();
    lane3->slot = b.r2;
    lane3->param = "Attack";
    lane3->setPoints({{2, 0.1}, {1, 0.9}});
    b.R->automation = std::vector<AutomationPtr>{lane3};

    b.mfx = slot("/usr/lib/vst3/Limiter.vst3", "Limiter", "effect", "lim");
    p.masterEffects = std::vector<SlotPtr>{b.mfx};
    p.masterCables = std::vector<Cable>{{End::in(), End::of(b.mfx.get())},
                                        {End::of(b.mfx.get()), End::out()},
                                        {End::source(b.B.get()), End::sidechain(b.mfx.get())}};

    auto src1 = tone(44100, 0.03, 0.5);
    b.c1 = makeAudioClip("drums.wav", src1, 44100, SR);
    b.c1->track = b.A;
    b.c1->start = 4800;
    b.c1->offset = 480;
    b.c1->length = 24000;
    b.c1->color = "#123456";
    b.c1->tone = "light";
    b.c2 = copyClip(b.c1);  // a split: the same audio
    b.c2->start = 96000;
    b.c2->offset = 0;
    b.c2->length = -1;
    b.c2->disabled = true;
    auto src2 = tone(30000, 0.011, 0.25);
    b.c3 = makeAudioClip("vocals: take/1?", src2, SR, SR);
    b.c3->track = b.B;
    auto src2copy = std::make_shared<AudioBuffer>(*src2);  // the same sound in another buffer
    b.c4 = makeAudioClip("vocals again", src2copy, SR, SR);
    b.c4->track = b.B;
    b.c4->start = 48000;
    b.mc = makeMidiClip("Pattern", {{0, 0.5, 60, 100}, {0.25, 1.0, 64, 90.5}}, 2.0, SR);
    b.mc->track = b.M;
    b.mc->start = 24000;
    b.mc->color = "#abcdef";
    for (auto& c : {b.c1, b.c2, b.c3, b.c4, b.mc}) p.addClip(c);

    b.C->frozen = makeAudioClip("Audio 5 (frozen)", tone(9600, 0.2, 0.1), SR, SR);
    // (no frozen->track, as the program does it: the track owns its rendering, a back pointer is a cycle)
    b.C->frozen->start = 1000;

    p.timing.bpm = 133.5;
    p.timing.beatsPerBar = 3;
    p.timing.thirds = true;
    p.timing.beats = false;
    p.timing.loopStart = 1.0;
    p.timing.loopEnd = 3.0;
    p.timing.looping = true;
}

static std::vector<std::string> keys(const Json& j) {
    std::vector<std::string> out;
    for (auto& m : j.members()) out.push_back(m.first);
    return out;
}

static AudioFileFn byPath() {
    return [](const Clip& c) { return util::baseName(c.sourcePath); };
}

static void testRoundTrip() {
    Built b;
    build(b);
    MasterSettings master;
    master.volumeDb = -6;
    master.mute = true;
    std::string path = kDir + "/Song.json", err;
    CHECK(saveProject(path, b.p, SR, master, err));
    std::string folder = audioFolder(path);
    CHECK(util::endsWith(folder, "/project/Song_audio"));
    CHECK(util::isDir(folder));
    CHECK(wavCount(folder) == 3);  // drums (shared by the split), vocals (identical audio), the frozen track
    CHECK(b.c1->sourcePath == b.c2->sourcePath);
    CHECK(b.c3->sourcePath == b.c4->sourcePath);
    CHECK(b.c1->sourcePath == util::joinPath(folder, "drums.wav"));
    CHECK(b.c3->sourcePath == util::joinPath(folder, "vocals_ take_1_.wav"));
    CHECK(b.C->frozen->sourcePath == util::joinPath(folder, "Audio 5 _frozen_.wav"));
    // the audio in the folder is exactly the clip's (32-bit float WAV at its own rate)
    SoundData sd;
    CHECK(readSoundFile(b.c1->sourcePath, sd, err));
    CHECK(sd.sampleRate == 44100 && sd.channels == 2 && sd.samples == b.c1->source->samples);

    // the JSON: the Python program's keys, order and types
    std::string text;
    CHECK(util::readFile(path, text));
    const Json j = Json::parse(text);
    CHECK((keys(j) == std::vector<std::string>{"format", "version", "master", "tracks_made", "timing", "markers", "groups",
                                               "tracks", "clips", "master_effects", "master_cables", "created",
                                               "audio_files"}));
    // (and, after the Python program's keys, when the project was made and each audio file's size and sound: to find it again if it goes missing)
    CHECK(j["audio_files"].size() == 3 && j["audio_files"]["Song_audio/drums.wav"].num("size", -1) == util::fileSize(b.c1->sourcePath));
    CHECK(j.str("format") == "exbit-project" && j["version"].toInt() == 1);
    CHECK(text.find("\"master\": {\n    \"volume_db\": -6,\n    \"mute\": true\n  }") != std::string::npos);
    CHECK(text.find("\"bpm\": 133.5") != std::string::npos);
    CHECK(text.find("\"beats_per_bar\": 3,") != std::string::npos);
    CHECK((keys(j["timing"]) == std::vector<std::string>{"bpm", "beats_per_bar", "beats", "thirds", "loop_start", "loop_end",
                                                         "looping", "changes", "grid", "snap_on", "key", "scale",
                                                         "timebase"}));  // (a new project: Beats)
    CHECK((keys(j["tracks_made"]) == std::vector<std::string>{"audio", "midi", "return", "group"}));
    CHECK(j["tracks_made"]["audio"].toInt() == 3 && j["tracks_made"]["midi"].toInt() == 2 && j["tracks_made"]["group"].toInt() == 1);
    CHECK((keys(j["groups"][size_t(0)]) == std::vector<std::string>{"name", "folded", "muted", "soloed", "color", "tone"}));
    const Json& tA = j["tracks"][size_t(0)];
    CHECK((keys(tA) == std::vector<std::string>{"name", "kind", "muted", "soloed", "output", "volume_db", "pan", "delay_ms",
                                                "height", "color", "tone", "needs_instrument", "show_automation", "uses_rack",
                                                "group", "frozen", "instrument", "effects", "cables", "rack", "rack_cables",
                                                "automation", "sends", "sampler", "midi_effects", "takes", "show_takes"}));
    CHECK(tA["group"].isNull() && tA["frozen"].isNull() && tA["instrument"].isNull());
    CHECK(tA["cables"].dump() == "[[\"in\", \"out\"]]");
    CHECK(tA["rack_cables"].dump() == "[]");
    CHECK(text.find("\"volume_db\": -3.5") != std::string::npos);
    CHECK(text.find("\"volume_db\": 0.0") != std::string::npos);  // floats stay floats
    CHECK(text.find("\"height\": 100,") != std::string::npos);     // ints stay ints
    const Json& tM = j["tracks"][size_t(2)];
    CHECK(tM["group"].toInt() == 0 && j["tracks"][size_t(1)]["group"].toInt() == 0);
    CHECK(tM["cables"].dump() == "[[\"in\", 0], [0, 1], [[\"track\", 0], [\"side\", 1]], [1, \"out\"]]");
    CHECK((keys(tM["instrument"]) == std::vector<std::string>{"path", "name", "kind", "plugin_name", "bypass", "volume_db", "mix",
                                                              "state"}));
    std::string state;
    CHECK(base64Decode(tM["instrument"].str("state"), state) && state == b.inst->state);
    CHECK(tM["effects"][size_t(0)]["at"].dump() == "[10, 20]");
    CHECK(tM["effects"][size_t(1)]["state"].isNull() && tM["effects"][size_t(1)]["plugin_name"].isNull());
    CHECK(tM["automation"].dump() ==
          "[{\"plugin\": 0, \"param\": \"Cutoff\", \"height\": 80, \"points\": [[0.0, 0.5], [1.5, 1.0], [3.0, 0.25]]}, "
          "{\"plugin\": \"instrument\", \"param\": \"Volume\", \"height\": 64, \"points\": [[0.5, 0.75]]}]");
    const Json& tR = j["tracks"][size_t(3)];
    CHECK(tR["rack_cables"].dump() == "[[\"in\", 0], [0, \"out\"], [\"in\", 1]]");
    CHECK(tR["automation"][size_t(0)]["plugin"].dump() == "[\"rack\", 1]");
    CHECK(tR["automation"][size_t(0)]["points"].dump() == "[[1.0, 0.9], [2.0, 0.1]]");  // sorted by time
    const Json& tC = j["tracks"][size_t(4)];
    CHECK(tC["frozen"].dump() == "{\"file\": \"Song_audio/Audio 5 _frozen_.wav\", \"start\": 0.020833333333333332}");
    CHECK(j["master_cables"].dump() == "[[\"in\", 0], [0, \"out\"], [[\"track\", 1], [\"side\", 0]]]");
    const Json& clip0 = j["clips"][size_t(0)];
    CHECK((keys(clip0) == std::vector<std::string>{"kind", "name", "track", "start", "offset", "length", "color", "tone",
                                                   "disabled", "file", "gain_db", "fade_in", "fade_out", "stretch", "pitch",
                                                   "source_bpm", "reverse", "loop", "loop_start", "loop_length"}));
    CHECK(clip0["start"].dump() == "0.1" && clip0["offset"].dump() == "0.01" && clip0["length"].dump() == "0.5");
    CHECK(clip0.str("file") == "Song_audio/drums.wav");
    CHECK(j["clips"][size_t(1)]["length"].isNull() && j["clips"][size_t(1)]["disabled"].toBool());
    CHECK(j["clips"][size_t(1)]["color"].str("x", "") == "" && j["clips"][size_t(1)].str("color") == "#123456");
    const Json& midi = j["clips"][size_t(4)];
    CHECK((keys(midi) == std::vector<std::string>{"kind", "name", "track", "start", "offset", "length", "color", "tone",
                                                  "disabled", "notes", "midi_length", "muted_notes", "loop"}));
    CHECK(midi["notes"] == Json::parse("[[0.0, 0.5, 60.0, 100.0], [0.25, 1.0, 64.0, 90.5]]"));
    CHECK(midi["midi_length"].dump() == "2.0" && midi["tone"].isNull() && midi["track"].toInt() == 2);

    // load into a fresh project
    Project q;
    MasterSettings m2;
    CHECK(loadProject(path, q, SR, m2, err));
    CHECK(m2 == master);
    CHECK(describe(q, SR, m2, byPath()) == describe(b.p, SR, master, byPath()));
    CHECK(describe(q, SR, m2, byPath()).dump(2) == describe(b.p, SR, master, byPath()).dump(2));
    CHECK(q.tracks.size() == 5 && q.clips.size() == 5);
    if (q.tracks.size() != 5 || q.clips.size() != 5) return;
    TrackPtr A = q.tracks[0], B = q.tracks[1], M = q.tracks[2], R = q.tracks[3], C = q.tracks[4];
    CHECK(A->name == "Audio 1" && A->color == "#ff0000" && A->tone == "dark" && A->volumeDb == -3.5 && A->muted &&
          A->height == 100 && A->output == 2 && !A->group);
    CHECK(B->group && B->group == M->group && B->group->name == "Group 1" && B->group->folded && B->group->muted &&
          B->group->color == "#00ff00" && B->group->tone == "light" && B->soloed);
    CHECK(M->kind == Kind::Midi && !M->needsInstrument && M->showAutomation && M->instrument);
    CHECK(M->instrument->state == b.inst->state && M->instrument->pluginName == "Synth A" && M->instrument->kind == "instrument");
    CHECK(M->effects.size() == 2);
    if (M->effects.size() == 2) {
        PluginSlot* f1 = M->effects[0].get();
        PluginSlot* f2 = M->effects[1].get();
        CHECK(f1->placed && f1->atX == 10 && f1->atY == 20 && f1->state == "abc");
        CHECK(!M->effects[1]->placed && M->effects[1]->bypass && M->effects[1]->state.empty());
        std::vector<Cable> want{{End::in(), End::of(f1)}, {End::of(f1), End::of(f2)},
                                {End::source(A.get()), End::sidechain(f2)}, {End::of(f2), End::out()}};
        CHECK(*M->cables == want);
        CHECK(M->automation.size() == 2 && M->automation[0]->slot == M->effects[0] && M->automation[1]->slot == M->instrument);
        CHECK(M->automation[0]->height == 80 && M->automation[0]->points.size() == 3 && M->automation[0]->points[1].t == 1.5);
    }
    CHECK(R->usesRack && R->rack.size() == 2);
    if (R->rack.size() == 2) {
        PluginSlot* i1 = R->rack[0].get();
        PluginSlot* i2 = R->rack[1].get();
        CHECK(i1->volumeDb == -6 && i1->placed && i1->atX == 300 && i1->atY == -40);
        std::vector<Cable> want{{End::in(), End::of(i1)}, {End::of(i1), End::out()}, {End::in(), End::of(i2)}};
        CHECK(*R->rackCables == want);
        CHECK(R->automation.size() == 1 && R->automation[0]->slot == R->rack[1]);
    }
    CHECK(q.masterEffects.size() == 1);
    if (q.masterEffects.size() == 1) {
        PluginSlot* mf = q.masterEffects[0].get();
        std::vector<Cable> want{{End::in(), End::of(mf)}, {End::of(mf), End::out()}, {End::source(B.get()), End::sidechain(mf)}};
        CHECK(*q.masterCables == want);
    }
    CHECK(q.pluginSlots().size() == 6);
    ClipPtr c1 = q.clips[0], c2 = q.clips[1], c3 = q.clips[2], c4 = q.clips[3], mc = q.clips[4];
    CHECK(c1->track == A && c1->start == 4800 && c1->offset == 480 && c1->length == 24000 && c1->color == "#123456" &&
          c1->tone == "light" && !c1->disabled && c1->name == "drums.wav");
    CHECK(c2->start == 96000 && c2->length == -1 && c2->disabled);
    CHECK(c1->source == c2->source && c3->source == c4->source);  // one file: loaded once, shared
    CHECK(c1->sourceSr == 44100 && c1->source->samples == b.c1->source->samples);
    CHECK(c1->data && c1->data->frames() == 48000);  // prepared at the engine rate
    CHECK(c1->peaks != nullptr);
    CHECK(c3->track == B && c4->start == 48000 && c4->name == "vocals again");
    CHECK(mc->kind == Kind::Midi && mc->track == M && mc->start == 24000 && mc->lengthS == 2.0 && mc->notes &&
          mc->notes->size() == 2 && (*mc->notes)[1].velocity == 90.5 && mc->data && mc->color == "#abcdef");
    CHECK(C->frozen && C->frozen->start == 1000 && C->frozen->name == "Audio 3 (frozen)" && !C->frozen->track &&
          C->frozen->source->samples == b.C->frozen->source->samples && C->frozen->data);
    CHECK(q.timing.bpm == 133.5 && q.timing.beatsPerBar == 3 && q.timing.thirds && !q.timing.beats &&
          q.timing.loopStart == 1.0 && q.timing.loopEnd == 3.0 && q.timing.looping);
    CHECK(q.tracksMade["audio"] == 3 && q.tracksMade["midi"] == 2 && q.tracksMade["group"] == 1);

    // saving again reuses the files (and writes the same JSON)
    CHECK(saveProject(path, q, SR, m2, err));
    CHECK(wavCount(folder) == 3);
    std::string text2;
    CHECK(util::readFile(path, text2) && text2 == text);
    // a project opens at any rate (times are seconds)
    Project q44;
    CHECK(loadProject(path, q44, 44100, m2, err));
    CHECK(q44.clips.size() == 5 && q44.clips[0]->start == 4410 && q44.clips[0]->offset == 441 &&
          q44.clips[0]->length == 22050 && q44.clips[0]->data->frames() == 44100);
    // saved elsewhere: a new folder of its own
    CHECK(saveProject(kDir + "/Other.json", q, SR, m2, err));
    CHECK(wavCount(kDir + "/Other_audio") == 3 && wavCount(folder) == 3);
    CHECK(q.clips[0]->sourcePath == util::joinPath(util::absolutePath(kDir + "/Other_audio"), "drums.wav"));
    // a different sound with a taken name gets a new name; nothing is overwritten
    auto other = makeAudioClip("drums.wav", tone(1000, 0.5, 0.5), SR, SR);
    other->track = A;
    b.p.addClip(other);
    CHECK(saveProject(path, b.p, SR, master, err));
    CHECK(wavCount(folder) == 4 && other->sourcePath == util::joinPath(folder, "drums (2).wav"));
    CHECK(readSoundFile(util::joinPath(folder, "drums.wav"), sd, err) && sd.samples == b.c1->source->samples);

    // unsaved-change detection: describe without states / with the default file names
    Json snap = describe(b.p, SR, master, nullptr, false);
    CHECK(snap == describe(b.p, SR, master, nullptr, false));
    CHECK(snap["tracks"][size_t(2)]["instrument"]["state"].isNull());
    b.c1->start += 1;
    CHECK(snap != describe(b.p, SR, master, nullptr, false));
    b.c1->start -= 1;
    MasterSettings louder = master;
    louder.volumeDb = -5;
    CHECK(snap != describe(b.p, SR, louder, nullptr, false));

    // missing audio: it opens, the clips that used it silent and marked missing (model/relink.h), named again on saving
    CHECK(saveProject(kDir + "/Miss.json", b.p, SR, master, err));
    CHECK(util::removeFile(kDir + "/Miss_audio/drums.wav"));
    CHECK(loadProject(kDir + "/Miss.json", q, SR, m2, err));
    int silent = 0;
    for (auto& c : q.clips)
        if (c->missing) {
            silent++;
            CHECK(c->missing->path == util::absolutePath(kDir + "/Miss_audio/drums.wav") && c->missing->size > 0);
        }
    CHECK(silent == 2 && q.clips.size() == 5);
    CHECK(saveProject(kDir + "/Miss.json", q, SR, m2, err));
    std::string saved;
    CHECK(util::readFile(kDir + "/Miss.json", saved) && saved.find(util::absolutePath(kDir + "/Miss_audio/drums.wav")) != std::string::npos);
    CHECK(!util::fileExists(kDir + "/Miss_audio/drums.wav"));  // (no silent stand-in written in its place)
}

static void testOldAndBroken() {
    std::string err;
    Project q;
    MasterSettings m;
    auto write = [](const std::string& name, const std::string& text) {
        std::string p = kDir + "/" + name;
        util::writeFile(p, text);
        return p;
    };
    CHECK(!loadProject(write("x.json", "{\"format\": \"something else\"}"), q, SR, m, err));
    CHECK(err == "This is not an Exbit project file.");
    // a plugin's path as projects saved on Windows had it, '\' and '/' mixed: read with '/' there
    SlotPtr mixed = readSlot(Json::parse(R"({"path": "C:/Program Files/Common Files/VST3/X.vst3/Contents\\x86_64-win/X.vst3"})"));
#ifdef _WIN32
    CHECK(mixed->path == "C:/Program Files/Common Files/VST3/X.vst3/Contents/x86_64-win/X.vst3");
#else
    CHECK(mixed->path.find('\\') != std::string::npos);  // (a Linux file's name may hold one)
#endif
    CHECK(!loadProject(write("y.json", "not json at all"), q, SR, m, err));
    CHECK(err == "This is not an Exbit project file.");
    CHECK(!loadProject(kDir + "/nothing-here.json", q, SR, m, err));
    CHECK(!loadProject(write("z.json", "{\"format\": \"exbit-project\"}"), q, SR, m, err));
    CHECK(err.find("damaged") != std::string::npos);
    CHECK(!loadProject(write("w.json", "{\"format\": \"exbit-project\", \"tracks\": [], \"clips\": [{\"kind\": \"midi\", "
                                       "\"name\": \"x\", \"track\": 3, \"start\": 0, \"offset\": 0, \"length\": null}]}"),
                       q, SR, m, err));
    // an old project: effects but no cables (wired one after the other), no rack cables, no timing, no master
    std::string old = R"({"format": "exbit-project", "version": 1,
      "tracks": [{"name": "Old", "kind": "midi", "effects": [
          {"path": "/a.vst3", "name": "A", "kind": "effect"}, {"path": "/b.vst3", "name": "B", "kind": "effect", "state": "eHl6"}],
        "rack": [{"path": "/p.vst3", "name": "P", "kind": "instrument"}],
        "automation": [{"plugin": 5, "param": "gone"}, {"plugin": 1, "param": "Mix", "points": [[2, 3], [-1, 0.5]]},
                       {"plugin": "instrument", "param": "x"}]}],
      "clips": [{"kind": "midi", "name": "M", "track": 0, "start": 0.5, "offset": 0, "length": null,
                 "notes": [[0, 1, 60, 100]], "midi_length": 1.5}],
      "master_effects": [{"path": "/m.vst3", "name": "M", "kind": "effect"}],
      "timing": {"bpm": 5000, "beats_per_bar": 0, "loop_start": 2.0, "loop_end": 2.05, "looping": true}})";
    CHECK(loadProject(write("old.json", old), q, SR, m, err));
    CHECK(m.volumeDb == 0 && !m.mute);
    CHECK(q.tracks.size() == 1 && q.clips.size() == 1);
    if (q.tracks.size() == 1) {
        const Track& t = *q.tracks[0];
        CHECK(t.name == "Old" && t.kind == Kind::Midi && t.height == config::TRACK_H && t.color.empty());
        CHECK(*t.cables == chain(*t.effects));
        CHECK(t.rackCables.empty());
        CHECK(t.effects.size() == 2 && t.effects[1]->state == "xyz");
        CHECK(t.automation.size() == 1 && t.automation[0]->param == "Mix" && t.automation[0]->slot == t.effects[1]);
        if (t.automation.size() == 1) {
            CHECK(t.automation[0]->points.size() == 2 && t.automation[0]->points[0].t == 0 &&
                  t.automation[0]->points[0].v == 0.5 && t.automation[0]->points[1].v == 1.0);  // kept within 0…1
            CHECK(t.automation[0]->height == config::AUTOMATION_H);
        }
    }
    CHECK(*q.masterCables == chain(*q.masterEffects));
    CHECK(q.clips.size() == 1 && q.clips[0]->start == 24000 && q.clips[0]->length == -1);
    CHECK(q.timing.bpm == config::DEFAULT_BPM && q.timing.beatsPerBar == config::DEFAULT_BEATS_PER_BAR);
    CHECK(q.timing.loopStart == 0 && q.timing.loopEnd == 0 && !q.timing.looping);  // too short a loop
    CHECK(q.tracksMade["audio"] == 0 && q.tracksMade["group"] == 0);
}

static void testParts() {
    // cables: wrong ones are left out
    std::vector<SlotPtr> fx{std::make_shared<PluginSlot>(), std::make_shared<PluginSlot>()};
    std::vector<TrackPtr> tracks{std::make_shared<Track>("T")};
    std::string err;
    Json entries = Json::parse(R"([["in", 0], ["in", 0], [0, "in"], ["out", 1], [0, 0], [0, ["side", 0]], [1, ["side", 0]],
        [["track", 0], 1], [["track", 0], ["side", 1]], [["track", 7], ["side", 1]], [["side", 0], 1], [0, 2], [0, 1.5],
        ["in"], [1, "out"], [["side", "in"], "out"], [["track", 0], "out"]])");
    std::vector<Cable> cables = readCables(entries, fx, tracks);
    std::vector<Cable> want{{End::in(), End::of(fx[0].get())},
                            {End::of(fx[1].get()), End::sidechain(fx[0].get())},
                            {End::source(tracks[0].get()), End::sidechain(fx[1].get())},
                            {End::of(fx[1].get()), End::out()}};
    CHECK(cables == want);
    CHECK(describeCables(cables, fx, tracks).dump() ==
          "[[\"in\", 0], [1, [\"side\", 0]], [[\"track\", 0], [\"side\", 1]], [1, \"out\"]]");
    CHECK(describeCables(cables, fx, {}).size() == 3);  // (from a track that is gone)
    CHECK(readCables(Json(), fx).size() == 3);           // none: a chain
    CHECK(readCables(Json::array(), fx).empty());
    // timing
    Timing t = readTiming(Json::parse(R"({"bpm": 90, "beats_per_bar": 7, "beats": false, "loop_start": -1, "loop_end": 4})"));
    CHECK(t.bpm == 90 && t.beatsPerBar == 7 && !t.beats && t.loopStart == 0 && t.loopEnd == 0);
    t = readTiming(Json::parse(R"({"loop_start": 1, "loop_end": 4, "looping": true})"));
    CHECK(t.loopStart == 1 && t.loopEnd == 4 && t.looping && t.bpm == config::DEFAULT_BPM);
    // fingerprints: by content and rate
    auto a = tone(1000, 0.1, 0.5);
    auto b = std::make_shared<AudioBuffer>(*a);
    auto c = tone(1000, 0.1, 0.25);
    CHECK(fingerprint(a, 48000) == fingerprint(b, 48000));
    CHECK(fingerprint(a, 48000) == fingerprint(a, 48000));
    CHECK(fingerprint(a, 48000) != fingerprint(a, 44100));
    CHECK(fingerprint(a, 48000) != fingerprint(c, 48000));
    // file names
    std::string dir = kDir + "/names";
    util::makeDirs(dir);
    CHECK(uniqueFileName(dir, "Kick 01.flac") == "Kick 01.wav");
    util::writeFile(dir + "/Kick 01.wav", "x");
    CHECK(uniqueFileName(dir, "Kick 01.flac") == "Kick 01 (2).wav");
    util::writeFile(dir + "/Kick 01 (2).wav", "x");
    CHECK(uniqueFileName(dir, "Kick 01") == "Kick 01 (3).wav");
    CHECK(uniqueFileName(dir, "a/b:c?.mp3") == "a_b_c_.wav");
    CHECK(uniqueFileName(dir, "  ...") == "_.wav");
    CHECK(uniqueFileName(dir, "") == "audio.wav");
    CHECK(uniqueFileName(dir, ".wav") == "_wav.wav");
    CHECK(uniqueFileName(dir, "Bass—line.wav") == "Bass_line.wav");      // (punctuation: a dash)
    CHECK(uniqueFileName(dir, "Grüße – Ωmega.wav") == "Grüße _ Ωmega.wav");  // (letters of any script kept)
    CHECK(audioFolder("/x/y/My Song.json") == util::absolutePath("/x/y") + "/My Song_audio");  // (Windows: C:/x/y)
    // master settings
    MasterSettings ms = MasterSettings::fromJson(Json::parse(R"({"volume_db": -12, "mute": true})"));
    CHECK(ms.volumeDb == -12 && ms.mute && ms.toJson().dump() == "{\"volume_db\": -12, \"mute\": true}");
    CHECK(MasterSettings::fromJson(Json()).volumeDb == 0);
}

// Fade shapes (model.h FADE SHAPES) and pre-fader sends (model.h Send): saved under keys of their own,
// written only when a fade is curved or a send is pre-fader, so a project without them saves as before.
static void testFadeShapesAndPreFader() {
    Project p;
    auto a = p.addTrack(Kind::Audio), b = p.addTrack(Kind::Audio), r = p.addTrack(Kind::Return);
    auto shaped = makeAudioClip("shaped", tone(SR, 0.03, 0.5), SR, SR);
    shaped->track = a;
    shaped->fadeIn = 4800;
    shaped->fadeOut = 2400;
    shaped->fadeInShape = FadeShape::Smooth;
    shaped->fadeOutShape = FadeShape::EqualPower;
    auto straight = makeAudioClip("straight", tone(SR, 0.02, 0.5), SR, SR);
    straight->track = b;
    straight->fadeIn = 480;
    auto outOnly = makeAudioClip("out only", tone(SR, 0.04, 0.5), SR, SR);
    outOnly->track = b;
    outOnly->start = SR * 2;
    outOnly->fadeOut = 960;
    outOnly->fadeOutShape = FadeShape::Slow;
    for (auto& c : {shaped, straight, outOnly}) p.addClip(c);
    a->sends = std::vector<Send>{{r, -6.0, true}};
    b->sends = std::vector<Send>{{r, -12.0}};
    std::string path = kDir + "/Shapes.json", err;
    MasterSettings master;
    CHECK(saveProject(path, p, SR, master, err));
    std::string text;
    CHECK(util::readFile(path, text));
    const Json j = Json::parse(text);
    const Json& c0 = j["clips"][size_t(0)];
    CHECK(c0.str("fade_in_shape") == "smooth" && c0.str("fade_out_shape") == "equal_power");
    const Json& c1 = j["clips"][size_t(1)];  // (straight fades: no keys, as before)
    CHECK(c1["fade_in_shape"].isNull() && c1["fade_out_shape"].isNull() && c1["fade_in"].toDouble() == 0.01);
    const Json& c2 = j["clips"][size_t(2)];
    CHECK(c2["fade_in_shape"].isNull() && c2.str("fade_out_shape") == "slow");
    CHECK(j["tracks"][size_t(0)]["pre_fader_sends"].dump() == "[2]");
    CHECK(j["tracks"][size_t(0)]["sends"].dump() == "[[2, -6.0]]");  // (as before: older versions read it)
    CHECK(j["tracks"][size_t(1)]["pre_fader_sends"].isNull() && j["tracks"][size_t(2)]["pre_fader_sends"].isNull());
    // loaded back: the same
    Project q;
    MasterSettings m2;
    CHECK(loadProject(path, q, SR, m2, err));
    CHECK(q.clips.size() == 3 && q.tracks.size() == 3);
    if (q.clips.size() == 3 && q.tracks.size() == 3) {
        CHECK(q.clips[0]->fadeInShape == FadeShape::Smooth && q.clips[0]->fadeOutShape == FadeShape::EqualPower);
        CHECK(q.clips[1]->fadeInShape == FadeShape::Linear && q.clips[1]->fadeOutShape == FadeShape::Linear);
        CHECK(q.clips[2]->fadeInShape == FadeShape::Linear && q.clips[2]->fadeOutShape == FadeShape::Slow);
        CHECK(q.tracks[0]->sends.size() == 1 && q.tracks[0]->sends[0].preFader && q.tracks[0]->sends[0].db == -6.0 &&
              q.tracks[0]->sends[0].track == q.tracks[2]);
        CHECK(q.tracks[1]->sends.size() == 1 && !q.tracks[1]->sends[0].preFader);
        CHECK(q.clips[0]->level(2400) == shaped->level(2400));
    }
    CHECK(describe(q, SR, m2, byPath()) == describe(p, SR, master, byPath()));
    // an older file (no shape keys, no pre-fader key): straight fades, sends after the fader
    const std::string oldTracks = R"("tracks": [{"name": "A", "kind": "audio", )";
    const std::string rest = R"("sends": [[1, -3.0]]}, {"name": "R", "kind": "return"}],
      "clips": [{"kind": "audio", "name": "s", "track": 0, "start": 0, "offset": 0, "length": null,
                 "file": "Shapes_audio/shaped.wav", "fade_in": 0.1, "fade_out": 0.05}]})";
    const std::string head = R"({"format": "exbit-project", "version": 1, )";
    util::writeFile(kDir + "/OldShapes.json", head + oldTracks + rest);
    Project o;
    CHECK(loadProject(kDir + "/OldShapes.json", o, SR, m2, err));
    CHECK(o.clips.size() == 1 && o.tracks.size() == 2);
    if (o.clips.size() == 1 && o.tracks.size() == 2) {
        CHECK(o.clips[0]->fadeIn == SR / 10 && o.clips[0]->fadeInShape == FadeShape::Linear &&
              o.clips[0]->fadeOutShape == FadeShape::Linear);
        CHECK(o.tracks[0]->sends.size() == 1 && !o.tracks[0]->sends[0].preFader);
    }
    // bad pre-fader entries (not an index, out of range, a track it doesn't send to) are left out
    util::writeFile(kDir + "/BadPre.json", head + oldTracks + R"("pre_fader_sends": [7, "x", 0], )" + rest);
    Project w;
    CHECK(loadProject(kDir + "/BadPre.json", w, SR, m2, err));
    CHECK(w.tracks.size() == 2 && w.tracks[0]->sends.size() == 1 && !w.tracks[0]->sends[0].preFader);
}

// when a project was made: set for a new one, saved as "created" (ISO-8601, UTC) and read back, kept by saving
// again and by saving elsewhere; an older file without it takes the file's own creation time
static void testCreatedTime() {
    int64_t before = (int64_t)std::time(nullptr);
    Project made;
    CHECK(made.createdUnixSeconds >= before && made.createdUnixSeconds <= (int64_t)std::time(nullptr));
    made.createdUnixSeconds = 1764072000;  // 2025-11-25 12:00:00 UTC
    Json described = describe(made, SR, {});
    CHECK(described["created"].toString() == "2025-11-25T12:00:00Z");

    std::string path = kDir + "/Created.json", err;
    MasterSettings master;
    CHECK(saveProject(path, made, SR, master, err));
    Project loaded;
    CHECK(loadProject(path, loaded, SR, master, err));
    CHECK(loaded.createdUnixSeconds == 1764072000);
    // saved again (a track more) and saved elsewhere ("Save as"): the same time
    loaded.addTrack(Kind::Midi);
    CHECK(saveProject(path, loaded, SR, master, err));
    std::string elsewhere = kDir + "/CreatedElsewhere.json";
    CHECK(saveProject(elsewhere, loaded, SR, master, err));
    Project again, other;
    CHECK(loadProject(path, again, SR, master, err) && again.createdUnixSeconds == 1764072000);
    CHECK(loadProject(elsewhere, other, SR, master, err) && other.createdUnixSeconds == 1764072000);
    // cleared (File → New): made now
    other.clear();
    CHECK(other.createdUnixSeconds >= before);

    // an older file without "created": the file's own creation time (else when it was last changed)
    // (a new name each run: NTFS gives a file made within 15 s of one deleted under its name that one's creation
    // time, "file tunneling", and the last run's was just cleared away)
    std::string older = kDir + "/NoCreated-" + std::to_string(before) + ".json";
    util::writeFile(older, R"({"format": "exbit-project", "version": 1, "tracks": [], "clips": []})");
    int64_t fileMade = file_birth_time::fileBirthTime(older);
    CHECK(fileMade >= before - 2 && fileMade <= (int64_t)std::time(nullptr) + 2);
    Project old;
    old.createdUnixSeconds = 5;
    CHECK(loadProject(older, old, SR, master, err));
    CHECK(old.createdUnixSeconds == fileMade);
    // a "created" that isn't a time: the file's time too
    util::writeFile(older, R"({"format": "exbit-project", "version": 1, "tracks": [], "clips": [], "created": "soon"})");
    CHECK(loadProject(older, old, SR, master, err) && old.createdUnixSeconds > 0);
    // no file: nothing known (0); a project not known to be made at a time saves no "created"
    CHECK(file_birth_time::fileBirthTime(kDir + "/nothing-here.json") == 0);
    made.createdUnixSeconds = 0;
    CHECK(!describe(made, SR, {}).has("created"));
}

int main() {
    util::makeDirs(kDir);
    clearDir(kDir);
    testRoundTrip();
    testOldAndBroken();
    testParts();
    testFadeShapesAndPreFader();
    testCreatedTime();
    TEST_MAIN_END();
}
