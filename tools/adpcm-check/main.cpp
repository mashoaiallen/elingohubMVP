// Host build of the firmware's ADPCM encoder: raw little-endian int16 PCM in, ADPCM WAV out.
//   g++ -O2 -I firmware/esp32_hub tools/adpcm-check/main.cpp firmware/esp32_hub/adpcm.cpp -o adpcm-encode
#include <cstdio>
#include <vector>

#include "adpcm.h"

int main(int argc, char **argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: adpcm-encode in.pcm out.wav\n");
    return 2;
  }
  FILE *in = std::fopen(argv[1], "rb");
  if (!in) return 1;
  std::vector<int16_t> pcm;
  unsigned char b[2];
  while (std::fread(b, 1, 2, in) == 2) pcm.push_back(static_cast<int16_t>(b[0] | (b[1] << 8)));
  std::fclose(in);
  std::vector<uint8_t> out(Adpcm::wavBytes(static_cast<uint32_t>(pcm.size())));
  size_t n = Adpcm::encodeWav(pcm.data(), static_cast<uint32_t>(pcm.size()), 16000, out.data());
  if (n != out.size()) return 3;
  FILE *o = std::fopen(argv[2], "wb");
  std::fwrite(out.data(), 1, n, o);
  std::fclose(o);
  return 0;
}
