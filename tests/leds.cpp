// SPDX-License-Identifier: AGPL-3.0-only
#include "surface/leds.h"
#include <cassert>
#include <vector>

static std::vector<uint8_t> bytes(const MidiPacket& packet) {
    return std::vector<uint8_t>(packet.bytes.begin(), packet.bytes.begin() + packet.size);
}

int main() {
    // Measured message shapes: pad index is the printed pad number minus one,
    // index 16 is the fader light, and colours are 7-bit.
    assert(bytes(surface_rgb_packet(0, {127, 0, 0})) ==
           std::vector<uint8_t>({0xf0,0x47,0x7f,0x5c,0x65,0x00,0x04,0x00,0x7f,0x00,0x00,0xf7}));
    assert(bytes(surface_rgb_packet(surface_fader_light, {200, 101, 1})) ==
           std::vector<uint8_t>({0xf0,0x47,0x7f,0x5c,0x65,0x00,0x04,0x10,0x7f,0x65,0x01,0xf7}));
    assert(bytes(surface_cc_packet(81, 2)) == std::vector<uint8_t>({0xb0,81,2}));

    // Which buttons light, and in which colours.
    unsigned lit = 0, red = 0;
    for (unsigned n = 0; n < surface_buttons.size(); ++n) {
        lit += button_light_supported(SurfaceButton(n), ButtonLight::Dim);
        red += button_light_supported(SurfaceButton(n), ButtonLight::RedBright);
    }
    assert(lit == 17 && red == 11);
    for (auto button : {SurfaceButton::Shift, SurfaceButton::Minus, SurfaceButton::Plus, SurfaceButton::TopLeft,
                        SurfaceButton::TopMiddle, SurfaceButton::TopRight, SurfaceButton::WheelPush})
        assert(!button_light_supported(button, ButtonLight::Dim) && !button_light_supported(button, ButtonLight::Off));
    for (auto button : {SurfaceButton::PadBank, SurfaceButton::NoteRepeat, SurfaceButton::Stop, SurfaceButton::Play,
                        SurfaceButton::SampleRecord, SurfaceButton::SeqRecord})
        assert(button_light_supported(button, ButtonLight::Bright) && !button_light_supported(button, ButtonLight::RedDim));
    assert(button_light_supported(SurfaceButton::TapTempo, ButtonLight::RedDim));

    SurfaceLights lights;
    std::vector<std::vector<uint8_t>> wire;
    bool accept = true;
    auto send = [&](const MidiPacket& packet) { if (!accept) return false; wire.push_back(bytes(packet)); return true; };

    // Nothing is sent until the application sets a light.
    assert(lights.update(send, 64) == 0 && wire.empty());
    assert(!lights.button(SurfaceButton::Shift, ButtonLight::Dim));
    assert(!lights.button(SurfaceButton::Stop, ButtonLight::RedBright));
    assert(!lights.pad(16, {1, 2, 3}) && !lights.bank(8, true));
    assert(!lights.meter(2, 1) && !lights.meter(0, 32));
    assert(lights.button(SurfaceButton::Stop, ButtonLight::Bright));
    assert(lights.bank(0, true) && lights.bank(7, true) && lights.bank(7, false));
    assert(lights.pad(15, {127, 127, 127}) && lights.fader({101, 101, 101}));
    assert(lights.update(send, 64) == 5);
    assert(wire[0] == std::vector<uint8_t>({0xb0,81,2}));
    assert(wire[1] == std::vector<uint8_t>({0xb0,57,127}) && wire[2] == std::vector<uint8_t>({0xb0,64,0}));
    assert(wire[3][7] == 15 && wire[3][8] == 127 && wire[4][7] == 16 && wire[4][9] == 101);

    // Unchanged lights are not resent; only the last requested state is.
    wire.clear();
    assert(lights.update(send, 64) == 0);
    lights.button(SurfaceButton::Stop, ButtonLight::Dim);
    lights.button(SurfaceButton::Stop, ButtonLight::Bright);
    assert(lights.update(send, 64) == 0 && wire.empty());

    // Meter strips: CC 83 left, 84 right, the value a mask of the five LEDs.
    wire.clear();
    assert(lights.meter(0, 0b00011) && lights.meter(1, surface_meter_all));
    assert(lights.update(send, 64) == 2);
    assert(wire[0] == std::vector<uint8_t>({0xb0, 83, 3}) && wire[1] == std::vector<uint8_t>({0xb0, 84, 31}));
    wire.clear();

    // A limit or a full queue leaves the rest for the next update.
    lights.all_off();
    assert(lights.update(send, 10) == 10);
    accept = false;
    assert(lights.update(send, 64) == 0 && wire.size() == 10);
    accept = true;
    // 17 buttons, 7 banks (H is already off), 2 meters and 17 RGB lights changed.
    assert(lights.update(send, 64) == 17 + 7 + 2 + 17 - 10);
    assert(lights.update(send, 64) == 0);

    // Release turns off only what the application set, and nothing if it set nothing.
    SurfaceLights fresh;
    fresh.release();
    assert(fresh.update(send, 64) == 0);
    fresh.pad(4, {10, 20, 30}); fresh.button(SurfaceButton::Play, ButtonLight::Bright); fresh.meter(1, 1);
    assert(fresh.update(send, 64) == 3);
    wire.clear();
    fresh.release();
    assert(fresh.update(send, 64) == 3 && wire.size() == 3);
    assert(wire[0] == std::vector<uint8_t>({0xb0, 82, 0}) && wire[1] == std::vector<uint8_t>({0xb0, 84, 0}) &&
           wire[2] == bytes(surface_rgb_packet(4, {})));
    { // Power: the pads at a share of their colours, the buttons at rest, each from the light's next setting.
        SurfaceLights low;
        std::vector<std::vector<uint8_t>> sent;
        auto out = [&](const MidiPacket& packet) { sent.push_back(bytes(packet)); return true; };
        low.scale(50); low.pad(0, {127, 3, 0});
        assert(low.update(out, 64) == 1 && sent.back() == std::vector<uint8_t>({0xf0,0x47,0x7f,0x5c,0x65,0x00,0x04,0x00,64,2,0,0xf7}));
        low.scale(10); low.pad(0, {127, 3, 0}); // a lit channel never rounds down to dark
        assert(low.update(out, 64) == 1 && sent.back() == std::vector<uint8_t>({0xf0,0x47,0x7f,0x5c,0x65,0x00,0x04,0x00,13,1,0,0xf7}));
        low.scale(250); low.pad(0, {200, 3, 0}); // over 100 %: as asked, still 7-bit
        assert(low.update(out, 64) == 1 && sent.back() == std::vector<uint8_t>({0xf0,0x47,0x7f,0x5c,0x65,0x00,0x04,0x00,127,3,0,0xf7}));
        low.rest(true);
        low.button(SurfaceButton::Play, ButtonLight::Bright); low.button(SurfaceButton::Mute, ButtonLight::RedBright);
        low.button(SurfaceButton::Stop, ButtonLight::Dim); low.button(SurfaceButton::Erase, ButtonLight::Off);
        sent.clear();
        assert(low.update(out, 64) == 4);
        for (const auto& message : sent) {
            uint8_t value = message[2];
            assert(value == uint8_t(ButtonLight::Dim) || value == uint8_t(ButtonLight::RedDim) || value == uint8_t(ButtonLight::Off));
        }
        low.rest(false); low.button(SurfaceButton::Play, ButtonLight::Bright);
        assert(low.update(out, 64) == 1 && sent.back()[2] == uint8_t(ButtonLight::Bright));
    }
}
