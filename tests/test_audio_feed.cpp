// The console's sound made ready for the audio device: 16-bit stereo, the
// device's speed, and the WAV recording.
#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>

#include "platform/audio_feed.h"
#include "temp_dir.h"
#include "test.h"

using bd::platform::AudioFeed;

TEST(audio_feed_makes_16_bits_around_the_resting_level) {
  AudioFeed feed;
  feed.add(0x200, 0x3FF);
  feed.add(0x000, 0x201);
  CHECK(feed.pairs_waiting() == 2);
  const std::vector<std::int16_t> taken = feed.take();
  CHECK(taken.size() == 4 && taken[0] == 0 && taken[1] == 511 * 64 && taken[2] == -32768 && taken[3] == 64);
  CHECK(feed.pairs_waiting() == 0 && feed.take().empty());
}

TEST(audio_feed_keeps_the_device_in_step) {
  CHECK(AudioFeed::speed(AudioFeed::kTargetQueued) == 1.0);
  CHECK(AudioFeed::speed(AudioFeed::kTargetQueued * 2) > 1.0 && AudioFeed::speed(0) < 1.0);
  CHECK(AudioFeed::speed(AudioFeed::kTargetQueued * 100) <= 1.005 && AudioFeed::speed(0) >= 0.995);
  CHECK(!AudioFeed::too_far_behind(AudioFeed::kTargetQueued * 2) && AudioFeed::too_far_behind(AudioFeed::kTargetQueued * 9));
}

TEST(audio_feed_records_a_wav_file) {
  bd_test::TempDir dir;
  const auto file = dir.path() / "sound.wav";
  {
    AudioFeed feed;
    std::string error;
    CHECK(feed.record(file, error));
    for (int i = 0; i < 3; ++i) feed.add(0x210, 0x1F0);
    feed.take();
    CHECK(feed.pairs_recorded() == 3);
  }
  std::ifstream in(file, std::ios::binary);
  const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(bytes.size() == 44 + 12);
  CHECK(bytes[0] == 'R' && bytes[8] == 'W' && bytes[40] == 12 && bytes[24] == (AudioFeed::kRate & 0xFF));
  // The first left sample: 16 above rest, times 64 = 1024.
  CHECK(bytes[44] == 0x00 && bytes[45] == 0x04);
}
