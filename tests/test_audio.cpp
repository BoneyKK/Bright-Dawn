// The program's sound (src/audio): the change of rate, the mixer, the music
// pack and its loops, a piece of music as it plays, and the replacement of
// the game's sequences. The one music file is tests/data/tone.ogg, made for
// the tests (a 440 Hz tone on the left, 660 Hz on the right, half a second
// at 44,100 a second, tagged LOOPSTART=4410 and LOOPLENGTH=11025).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <vector>

#include "audio/mixer.h"
#include "audio/music_pack.h"
#include "audio/music_replacer.h"
#include "audio/music_voice.h"
#include "audio/resampler.h"
#include "audio/vorbis_file.h"
#include "temp_dir.h"
#include "test.h"

using namespace bd::audio;

namespace {

const std::filesystem::path kTone = std::filesystem::path(BD_TEST_DATA) / "tone.ogg";
constexpr double kPi = 3.14159265358979323846;

// A source whose frame n is (n, -n): where a voice is in it can be read
// from what it gives.
class Counting final : public Source {
 public:
  Counting(int rate, std::uint64_t length) : rate_(rate), length_(length) {}
  int rate() const override { return rate_; }
  std::uint64_t length() const override { return length_; }
  std::size_t read(float* stereo, std::size_t frames) override {
    std::size_t n = 0;
    for (; n < frames && at_ < length_; ++n, ++at_) {
      stereo[n * 2] = static_cast<float>(at_);
      stereo[n * 2 + 1] = -static_cast<float>(at_);
    }
    return n;
  }
  bool seek(std::uint64_t frame) override {
    at_ = frame;
    return frame <= length_;
  }

 private:
  int rate_;
  std::uint64_t length_, at_ = 0;
};

std::vector<float> sine(double frequency, double rate, std::size_t frames) {
  std::vector<float> s(frames * 2);
  for (std::size_t i = 0; i < frames; ++i) {
    const auto v = static_cast<float>(std::sin(2 * kPi * frequency * static_cast<double>(i) / rate));
    s[i * 2] = v;
    s[i * 2 + 1] = v * 0.5f;
  }
  return s;
}

float peak(const std::vector<float>& s, std::size_t from, std::size_t to, int side = 0) {
  float p = 0;
  for (std::size_t i = from; i < to; ++i) p = std::max(p, std::abs(s[i * 2 + side]));
  return p;
}

// Frames of a voice, as a mixer at the source's own rate gives them.
std::vector<float> frames_of(MusicVoice& v, std::size_t n) {
  std::vector<float> out(n * 2, 0.0f);
  v.add_to(out.data(), n);
  return out;
}

}  // namespace

TEST(resampler_passes_samples_unchanged_at_the_same_rate) {
  Resampler r(48000, 48000);
  const std::vector<float> in = sine(1000, 48000, 100);
  r.push(in.data(), 100);
  std::vector<float> out;
  CHECK(r.pull(out, 1000) == 100);
  CHECK(out == in);
  CHECK(r.needed_for(5) == 5);
}

TEST(resampler_keeps_a_level_and_a_tone_from_the_console_rate_to_48000) {
  Resampler r(Mixer::kConsoleRate, Mixer::kRate);
  const std::vector<float> level(Mixer::kConsoleRate * 2, 0.5f);
  r.push(level.data(), Mixer::kConsoleRate);
  std::vector<float> out;
  const std::size_t made = r.pull(out, static_cast<std::size_t>(-1));
  // A second's input, less the 16 samples it waits for.
  CHECK(made > 47900 && made <= 48000);
  for (std::size_t i = 100; i < made; ++i) CHECK(std::abs(out[i * 2] - 0.5f) < 1e-4f);

  Resampler t(Mixer::kConsoleRate, Mixer::kRate);
  const std::vector<float> tone = sine(1000, Mixer::kConsoleRate, Mixer::kConsoleRate);
  t.push(tone.data(), Mixer::kConsoleRate);
  out.clear();
  const std::size_t n = t.pull(out, static_cast<std::size_t>(-1));
  CHECK(std::abs(peak(out, 1000, n) - 1.0f) < 0.01f);
  CHECK(std::abs(peak(out, 1000, n, 1) - 0.5f) < 0.01f);
  // A thousand periods: two thousand crossings of zero.
  int crossings = 0;
  for (std::size_t i = 1001; i < n; ++i) crossings += (out[(i - 1) * 2] < 0) != (out[i * 2] < 0);
  CHECK(crossings > 1900 && crossings < 2000);
}

TEST(resampler_takes_out_what_the_lower_rate_cannot_hold) {
  // 20 kHz, above the console rate's Nyquist frequency (16,364 Hz).
  Resampler r(Mixer::kRate, Mixer::kConsoleRate);
  const std::vector<float> tone = sine(20000, Mixer::kRate, Mixer::kRate / 4);
  r.push(tone.data(), Mixer::kRate / 4);
  std::vector<float> out;
  const std::size_t n = r.pull(out, static_cast<std::size_t>(-1));
  CHECK(n > 7000);
  CHECK(peak(out, 100, n) < 0.01f);
  // Input in pieces gives the same output as all at once.
  Resampler a(Mixer::kConsoleRate, Mixer::kRate), b(Mixer::kConsoleRate, Mixer::kRate);
  const std::vector<float> in = sine(440, Mixer::kConsoleRate, 3000);
  std::vector<float> whole, pieces;
  a.push(in.data(), 3000);
  a.pull(whole, static_cast<std::size_t>(-1));
  for (std::size_t at = 0; at < 3000; at += 137) {
    b.push(in.data() + at * 2, std::min<std::size_t>(137, 3000 - at));
    b.pull(pieces, static_cast<std::size_t>(-1));
  }
  CHECK(whole.size() == pieces.size());
  bool same = true;
  for (std::size_t i = 0; i < std::min(whole.size(), pieces.size()); ++i) same = same && std::abs(whole[i] - pieces[i]) < 1e-6f;
  CHECK(same);
}

TEST(music_pack_reads_places_and_names) {
  CHECK(parse_place("441000")->frame(44100) == 441000);
  CHECK(parse_place(" 10s ")->frame(44100) == 441000);
  CHECK(parse_place("10.5s")->frame(48000) == 504000);
  CHECK(parse_place("1:02.5")->frame(48000) == 3000000);
  CHECK(!parse_place("") && !parse_place("1.5") && !parse_place("abc") && !parse_place("1:75") && !parse_place("-3"));
  CHECK(sequence_key("map1") == "SEQ_MAP1" && sequence_key("seq_Map1") == "SEQ_MAP1" && sequence_key(" SEQ_MAP1 ") == "SEQ_MAP1");
}

TEST(music_pack_finds_files_by_name_and_as_its_manifest_says) {
  const std::filesystem::path folder = "pack";
  const std::vector<std::filesystem::path> files = {folder / "SEQ_MAP1.ogg", folder / "theme.OGG", folder / "Battle.ogg",
                                                    folder / "music.ini", folder / "notes.txt"};
  std::vector<std::string> problems;
  const MusicPack pack = MusicPack::from(folder, R"(
; A pack for the tests.
[pack]
volume = 50

[THEME1]            ; without SEQ_
file = theme.ogg    # in another case
loop_start = 1.5s
loop_end = 1:00
volume = 80

[SEQ_SHORI]
file = battle.ogg
loop = no

[SEQ_NOTHING]
loop = yes
nonsense
colour = red
)",
                                         files, problems);
  // SEQ_MAP1, THEME, BATTLE by name; THEME1 and SHORI by the manifest.
  CHECK(pack.size() == 5);
  const PackTrack* map = pack.find("map1");
  REQUIRE(map != nullptr);
  CHECK(map->file == folder / "SEQ_MAP1.ogg" && !map->loop_start && map->volume == 0.5);
  const PackTrack* theme = pack.find("SEQ_THEME1");
  REQUIRE(theme != nullptr);
  CHECK(theme->file == folder / "theme.OGG");
  CHECK(theme->loop_start->frame(48000) == 72000 && theme->loop_end->frame(48000) == 2880000);
  CHECK(std::abs(theme->volume - 0.4) < 1e-9);
  const PackTrack* shori = pack.find("SEQ_SHORI");
  REQUIRE(shori != nullptr);
  CHECK(shori->file == folder / "Battle.ogg" && shori->loop == false);
  CHECK(pack.find("SEQ_NOTHING") == nullptr && pack.find("SEQ_OTHER") == nullptr);
  // The line without "=", the unknown key, and the section without a file.
  CHECK(problems.size() == 3);
}

TEST(music_pack_takes_the_loop_from_the_pack_then_the_tags_then_the_whole_file) {
  std::string problem;
  PackTrack t;
  Loop l = resolve_loop(t, 44100, 100000, {}, problem);
  CHECK(l.on && l.start == 0 && l.end == 100000 && problem.empty());
  l = resolve_loop(t, 44100, 100000, {{"LOOPSTART", "4410"}, {"LOOPLENGTH", "11025"}}, problem);
  CHECK(l.start == 4410 && l.end == 15435);
  l = resolve_loop(t, 44100, 100000, {{"LOOPSTART", "4410"}, {"LOOPEND", "90000"}}, problem);
  CHECK(l.start == 4410 && l.end == 90000);
  // An end past the file's is the file's.
  l = resolve_loop(t, 44100, 100000, {{"LOOP_START", "10"}, {"LOOP_END", "200000"}}, problem);
  CHECK(l.start == 10 && l.end == 100000);
  // The pack's loop is used in place of the tags'.
  t.loop_start = Place{1.0, true};
  l = resolve_loop(t, 44100, 100000, {{"LOOPSTART", "4410"}, {"LOOPEND", "90000"}}, problem);
  CHECK(l.start == 44100 && l.end == 100000);
  t.loop_end = Place{50000, false};
  t.loop = false;
  l = resolve_loop(t, 44100, 100000, {}, problem);
  CHECK(!l.on && l.start == 44100 && l.end == 50000);
  // A loop that ends before it starts.
  t.loop_end = Place{100, false};
  l = resolve_loop(t, 44100, 100000, {}, problem);
  CHECK(l.start == 0 && l.end == 100000 && !problem.empty());
}

TEST(music_voice_loops_exactly_where_its_loop_says) {
  MusicVoice v(std::make_unique<Counting>(48000, 1000), Loop{true, 10, 20}, 48000);
  const std::vector<float> out = frames_of(v, 45);
  bool exact = true;
  for (std::size_t i = 0; i < 45; ++i) {
    const float expected = i < 20 ? static_cast<float>(i) : static_cast<float>(10 + (i - 20) % 10);
    exact = exact && out[i * 2] == expected && out[i * 2 + 1] == -expected;
  }
  CHECK(exact);
  CHECK(!v.done());
  // Told to finish, it plays on past the loop to the end of the file.
  v.finish();
  std::vector<float> rest = frames_of(v, 2000);
  CHECK(rest[0] == 15.0f && rest[(1000 - 16) * 2] == 999.0f && rest[(1000 - 15) * 2] == 0.0f);
  CHECK(v.done());
}

TEST(music_voice_plays_once_pauses_and_stops_smoothly) {
  MusicVoice once(std::make_unique<Counting>(48000, 30), Loop{false, 0, 30}, 48000);
  std::vector<float> out = frames_of(once, 40);
  CHECK(out[29 * 2] == 29.0f && out[30 * 2] == 0.0f && once.done());

  // A source at 1 everywhere, to see the gain.
  struct Level final : Source {
    int rate() const override { return 48000; }
    std::uint64_t length() const override { return 1000000; }
    std::size_t read(float* s, std::size_t n) override {
      std::fill(s, s + n * 2, 1.0f);
      at += n;
      return n;
    }
    bool seek(std::uint64_t f) override {
      at = f;
      return true;
    }
    std::uint64_t at = 0;
  };
  MusicVoice v(std::make_unique<Level>(), Loop{true, 0, 1000000}, 48000);
  v.set_gain(0.5f);
  out = frames_of(v, 10);
  CHECK(out[0] == 0.5f && out[18] == 0.5f);  // The first gain is there at once.
  // Softer over 10 ms (480 frames), not at once.
  v.set_gain(0.25f);
  out = frames_of(v, 600);
  CHECK(out[0] < 0.5f && out[0] > 0.49f && std::abs(out[60 * 2] - 0.375f) < 0.01f && out[500 * 2] == 0.25f);
  // Paused: quiet over 10 ms, then the source is not read.
  v.set_paused(true);
  frames_of(v, 600);
  const std::uint64_t paused_at = v.position();
  out = frames_of(v, 600);
  CHECK(out[0] == 0.0f && v.position() == paused_at && !v.done());
  v.set_paused(false);
  out = frames_of(v, 600);
  CHECK(out[0] > 0.0f && out[0] < 0.01f && out[590 * 2] == 0.25f && v.position() > paused_at);
  v.stop();
  out = frames_of(v, 600);
  CHECK(out[590 * 2] == 0.0f && v.done());
}

TEST(vorbis_file_reads_tags_and_seeks_to_the_sample) {
  auto opened = VorbisFile::open(kTone);
  REQUIRE(opened.ok());
  VorbisFile& f = *opened.value();
  CHECK(f.rate() == 44100 && f.channels() == 2 && f.length() == 22050);
  CHECK(f.tags().at("LOOPSTART") == "4410" && f.tags().at("LOOPLENGTH") == "11025");
  std::vector<float> all(22050 * 2);
  CHECK(f.read(all.data(), 22050) == 22050);
  float buf[2];
  CHECK(f.read(buf, 1) == 0);
  // 440 Hz on the left, 660 on the right, each at about full scale/8 (ffmpeg's sine).
  CHECK(peak(all, 1000, 21000) > 0.1f && peak(all, 1000, 21000, 1) > 0.1f);
  int crossings = 0;
  for (std::size_t i = 1; i < 22050; ++i) crossings += (all[(i - 1) * 2] < 0) != (all[i * 2] < 0);
  CHECK(crossings > 430 && crossings < 450);
  // From a seek, the same samples as read straight through.
  REQUIRE(f.seek(10000));
  std::vector<float> after(100 * 2);
  CHECK(f.read(after.data(), 100) == 100);
  bool same = true;
  for (std::size_t i = 0; i < 200; ++i) same = same && std::abs(after[i] - all[10000 * 2 + i]) < 1e-5f;
  CHECK(same);
  // Not a Vorbis file, and no file.
  bd_test::TempDir dir;
  dir.write("bad.ogg", "OggS this is not one");
  CHECK(!VorbisFile::open(dir.path() / "bad.ogg").ok());
  CHECK(!VorbisFile::open(dir.path() / "none.ogg").ok());
}

TEST(mixer_paces_music_by_the_console_and_records_the_mix) {
  bd_test::TempDir dir;
  const auto file = dir.path() / "mix.wav";
  Mixer m;
  std::string error;
  REQUIRE(m.record(file, error));
  // A frame of silence from the console, with a piece of music beside it.
  for (int i = 0; i < 546; ++i) m.add_console(0.0f, 0.0f);
  auto voice = std::make_shared<MusicVoice>(std::make_unique<Counting>(48000, 10), Loop{false, 0, 10}, Mixer::kRate);
  voice->set_gain(0.01f);
  m.play(voice);
  const std::vector<float> first = m.mix();
  std::size_t frames = first.size() / 2;
  CHECK(frames > 770 && frames <= 800);
  CHECK(std::abs(first[5 * 2] - 0.05f) < 1e-6f && std::abs(first[5 * 2 + 1] + 0.05f) < 1e-6f && first[10 * 2] == 0.0f);
  CHECK(m.console_waiting() == 0 && m.voices() == 0 && voice->done());
  // A second of the console's sound: a second at 48,000.
  for (int i = 0; i < Mixer::kConsoleRate; ++i) m.add_console(0.25f, -0.25f);
  const std::vector<float> mixed = m.mix();
  frames += mixed.size() / 2;
  CHECK(mixed.size() / 2 > 47900 && mixed.size() / 2 <= 48100);
  CHECK(std::abs(mixed[1000 * 2] - 0.25f) < 1e-4f && std::abs(mixed[1000 * 2 + 1] + 0.25f) < 1e-4f);
  // Limited to full scale.
  for (int i = 0; i < 1000; ++i) m.add_console(3.0f, -3.0f);
  const std::vector<float> loud = m.mix();
  CHECK(*std::max_element(loud.begin(), loud.end()) == 1.0f && *std::min_element(loud.begin(), loud.end()) == -1.0f);
  m.stop_recording();
  std::ifstream in(file, std::ios::binary);
  const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(bytes.size() == 44 + (frames + loud.size() / 2) * 4);
  CHECK(bytes[0] == 'R' && bytes[8] == 'W' && bytes[24] == (Mixer::kRate & 0xFF) && bytes[25] == (Mixer::kRate >> 8));

  CHECK(Mixer::speed(Mixer::kTargetQueued) == 1.0);
  CHECK(Mixer::speed(Mixer::kTargetQueued * 2) > 1.0 && Mixer::speed(0) < 1.0);
  CHECK(Mixer::speed(Mixer::kTargetQueued * 100) <= 1.005 && Mixer::speed(0) >= 0.995);
  CHECK(!Mixer::too_far_behind(Mixer::kTargetQueued * 2) && Mixer::too_far_behind(Mixer::kTargetQueued * 9));
}

TEST(sequence_catalog_tells_a_sequence_by_its_events) {
  const std::vector<std::uint8_t> map1 = {0xFE, 0x03, 0x00, 0x93, 0x01, 0x10, 0x00, 0x00, 0xC1, 0x7F, 0xE1, 0x78,
                                          0x00, 0x3C, 0x7F, 0x30, 0x80, 0x30, 0xFF, 0x11};
  std::vector<std::uint8_t> map2 = map1;
  map2.back() = 0x22;  // The same first 16 bytes; the rest differs.
  const std::vector<std::uint8_t> jingle = {0x3C, 0x7F, 0x30, 0xFF};
  std::vector<bd::rom::SdatSequence> list(4);
  list[0].name = "SEQ_MAP1";
  list[0].data = map1;
  list[1].name = "SEQ_MAP2";
  list[1].data = map2;
  list[2].name = "SEQ_JINGLE";
  list[2].data = jingle;
  list[3].name = "";  // Without a name: never told.
  list[3].data = jingle;
  const SequenceCatalog catalog(list);
  CHECK(catalog.size() == 3);
  std::vector<std::uint8_t> memory(256, 0xAA);
  std::copy(map2.begin(), map2.end(), memory.begin() + 40);
  std::copy(jingle.begin(), jingle.end(), memory.begin() + 100);
  CHECK(catalog.identify(memory, 40) == "SEQ_MAP2");
  CHECK(catalog.identify(memory, 100) == "SEQ_JINGLE");
  CHECK(catalog.identify(memory, 0).empty() && catalog.identify(memory, 1000).empty());
  // At the end of memory, cut short: not that sequence.
  CHECK(catalog.identify(std::vector<std::uint8_t>(memory.begin(), memory.begin() + 50), 40).empty());
}

TEST(music_replacer_follows_the_sequence_it_replaces) {
  bd_test::TempDir dir;
  std::filesystem::copy_file(kTone, dir.path() / "seq_test.ogg");
  std::vector<std::string> problems;
  MusicPack pack = MusicPack::scan(dir.path(), problems);
  CHECK(problems.empty() && pack.size() == 1);
  const std::vector<std::uint8_t> events = {0xFE, 0x01, 0x00, 0x3C, 0x7F, 0x30, 0xFF, 0x01,
                                            0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
  std::vector<bd::rom::SdatSequence> list(2);
  list[0].name = "SEQ_TEST";
  list[0].data = events;
  list[1].name = "SEQ_OTHER";
  list[1].data = std::span<const std::uint8_t>(events).subspan(3);
  MusicReplacer music(SequenceCatalog(list), std::move(pack));
  std::vector<std::uint8_t> memory(64, 0);
  std::copy(events.begin(), events.end(), memory.begin() + 8);

  Mixer mixer;
  std::array<PlayerState, MusicReplacer::kPlayers> players{};
  const auto frame = [&] {
    for (int i = 0; i < 546; ++i) mixer.add_console(0, 0);
    music.update(players, 1.0f, mixer);
    return mixer.mix();
  };
  // A sequence it has no file for: heard as it is.
  CHECK(!music.prepared(0, 1, memory, 11));
  players[0] = PlayerState{true, true, false, 1, 0, 0};
  frame();
  CHECK(mixer.voices() == 0 && music.playing(0).empty());
  // The one it has: silent, and the file plays once the sequence plays.
  CHECK(music.prepared(0, 2, memory, 8));
  players[0] = PlayerState{true, false, false, 2, 0, 0};
  frame();
  CHECK(mixer.voices() == 0 && music.playing(0) == "SEQ_TEST");
  players[0].playing = true;
  frame();
  std::vector<float> heard = frame();
  CHECK(mixer.voices() == 1 && peak(heard, 0, heard.size() / 2) > 0.02f);
  // The game's fader at -40 dB: a hundredth.
  players[0].decibels = -400;
  for (int i = 0; i < 3; ++i) frame();
  const float full = peak(heard, 0, heard.size() / 2);
  heard = frame();
  CHECK(peak(heard, 0, heard.size() / 2) < full / 50);
  // Stopped by the game: quiet, then gone.
  players[0].prepared = players[0].playing = false;
  frame();
  frame();
  CHECK(mixer.voices() == 0 && music.playing(0).empty());
  // Ended by itself: it plays on to the end of its file (half a second).
  CHECK(music.prepared(0, 3, memory, 8));
  players[0] = PlayerState{true, true, false, 3, 0, 0};
  frame();
  players[0] = PlayerState{false, false, false, 3, 1, 0};
  frame();
  CHECK(mixer.voices() == 1);
  for (int i = 0; i < 40; ++i) frame();
  CHECK(mixer.voices() == 0);
  // A file that cannot be read: the sequence is to be heard after all.
  std::ofstream(dir.path() / "seq_test.ogg", std::ios::binary | std::ios::trunc) << "nothing";
  CHECK(music.prepared(2, 1, memory, 8));
  players[2] = PlayerState{true, true, false, 1, 0, 0};
  frame();
  CHECK(music.take_failed() == 4u && music.take_failed() == 0 && mixer.voices() == 0);
}
