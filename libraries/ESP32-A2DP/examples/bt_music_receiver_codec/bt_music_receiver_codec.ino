/*
  Streaming Music from Bluetooth - with AAC decode support

  Copyright (C) 2020 Phil Schatzmann
  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// ==> Example A2DP Receiver which uses I2S to an external DAC, with SBC and
// AAC decoded via the audio-tools library instead of the stock SBC-only
// decoded path.
//
// Each add_decoder() call registers its own stream endpoint; which codec a
// connected source ends up using depends on what you've registered here
// plus what the source and the underlying ESP-IDF version support. Decoded
// PCM flows through the same output pipeline (I2S/AudioTools, volume,
// stream_reader) as the built-in SBC path - no extra wiring needed beyond
// add_decoder().
//
// A2DPDecoderSBC/A2DPDecoderAAC wrap whatever audio_tools::AudioDecoder you
// give them - they don't hard-depend on any specific decoder library, so
// you pick the implementation. This example uses:
//  - https://github.com/pschatzmann/arduino-libsbc      (audio_tools::SBCDecoder)
//  - https://github.com/pschatzmann/arduino-libhelix    (audio_tools::AACDecoderHelix)
// Install whichever decoder library you choose alongside this library.
//
// Registering a non-SBC (e.g. AAC) stream endpoint additionally requires
// ESP-IDF >= 6.1 at the Bluedroid stack level - on older IDF the AAC
// registration is expected to fail (logged via ESP_A2D_SEP_REG_STATE_EVT)
// and the source will negotiate SBC instead, which still works fine.
//
// bck = 14
// ws = 15
// data_out = 22

#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecSBC.h"
#include "AudioTools/AudioCodecs/CodecAACHelix.h"
#include "BluetoothA2DPSink.h"
#include "A2DPDecoderSBC.h"
#include "A2DPDecoderAAC.h"

I2SStream out;
BluetoothA2DPSink a2dp_sink(out);

SBCDecoder sbc_decoder;
A2DPDecoderSBC a2dp_sbc(sbc_decoder);

AACDecoderHelix aac_decoder;
A2DPDecoderAAC a2dp_aac(aac_decoder);

void setup() {
  Serial.begin(115200);
  a2dp_sink.add_decoder(a2dp_sbc);
  a2dp_sink.add_decoder(a2dp_aac);
  a2dp_sink.start("MyMusic");
}

void loop() {
  delay(1000);  // do nothing
}
