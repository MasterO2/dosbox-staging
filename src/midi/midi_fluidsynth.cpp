/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Copyright (C) 2002-2011  The DOSBox Team
 *  Copyright (C) 2020       Nikos Chantziaras <realnc@gmail.com>
 *  Copyright (C) 2020       The dosbox-staging team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "midi_fluidsynth.h"

#if C_FLUIDSYNTH

#include <cassert>
#include <string>

#include "control.h"
#include "cross.h"

MidiHandlerFluidsynth instance;

static void init_fluid_dosbox_settings(Section_prop &secprop)
{
	constexpr auto when_idle = Property::Changeable::WhenIdle;

	auto *str_prop = secprop.Add_string("soundfont", when_idle, "");
	str_prop->Set_help(
	        "Path to a SoundFont file in .sf2 format to use with FluidSynth.");

	// TODO Handle storing soundfonts in specific directory and update
	// the documentation; right now users need to specify full path or
	// fall on undocumented FluidSynth internal algorithm for picking
	// sf2 files.

	auto *int_prop = secprop.Add_int("fluid_rate", when_idle, 44100);
	int_prop->SetMinMax(8000, 96000);
	int_prop->Set_help(
	        "The sample rate of the audio generated by the synthesizer.\n"
	        "(min 8000, max 96000)");

	int_prop = secprop.Add_int("synth_threads", when_idle, 1);
	int_prop->SetMinMax(1, 256);
	int_prop->Set_help(
	        "If set to a value greater than 1, then additional synthesis\n"
	        "threads will be created to take advantage of many CPU cores.\n"
	        "(min 1, max 256)");
}

// SetMixerVolume is a callback that's given the user-desired mixer volume,
// which is a floating point multiplier that we apply internally as
// FluidSynth's gain value. We then read-back the gain, and use that to
// derive a pre-scale volume.
void MidiHandlerFluidsynth::SetMixerVolume(const AudioFrame<float> &desired_volume) noexcept
{
	double gain = static_cast<double>(std::min(desired_volume.left, desired_volume.right));
	fluid_settings_setnum(settings.get(), "synth.gain", gain);
	fluid_settings_getnum(settings.get(), "synth.gain", &gain);

	const float gain_f = static_cast<float>(gain);
	prescale_volume = {INT16_MAX * (desired_volume.left / gain_f),
	                   INT16_MAX * (desired_volume.right / gain_f)};

	// Finally, we keep track of the as-is external mixer volume, which is
	// used by the Soft Limiter when making mixer level recommendations.
	mixer_volume = desired_volume;
}

bool MidiHandlerFluidsynth::Open(MAYBE_UNUSED const char *conf)
{
	Close();

	fluid_settings_ptr_t fluid_settings(new_fluid_settings(),
	                                    delete_fluid_settings);
	if (!fluid_settings) {
		LOG_MSG("MIDI: new_fluid_settings failed");
		return false;
	}

	auto *section = static_cast<Section_prop *>(control->GetSection("fluidsynth"));

	// Detailed explanation of all available FluidSynth settings:
	// http://www.fluidsynth.org/api/fluidsettings.xml

	// Configure the settings and move it into the member
	const int sample_rate = section->Get_int("fluid_rate");
	fluid_settings_setnum(fluid_settings.get(), "synth.sample-rate", sample_rate);

	const int cpu_cores = section->Get_int("synth_threads");
	fluid_settings_setint(fluid_settings.get(), "synth.cpu-cores", cpu_cores);
	fluid_settings_setstr(fluid_settings.get(), "audio.sample-format", "float");
	settings = std::move(fluid_settings);

	// Create the fluid-synth object move it into the member
	fsynth_ptr_t fluid_synth(new_fluid_synth(settings.get()),
	                         delete_fluid_synth);
	if (!fluid_synth) {
		LOG_MSG("MIDI: Failed to create the FluidSynth synthesizer");
		return false;
	}
	synth = std::move(fluid_synth);

	// Load the SoundFont
	std::string soundfont = section->Get_string("soundfont");
	Cross::ResolveHomedir(soundfont);
	if (!soundfont.empty() && fluid_synth_sfcount(synth.get()) == 0) {
		fluid_synth_sfload(synth.get(), soundfont.data(), true);
	}
	DEBUG_LOG_MSG("MIDI: FluidSynth loaded %d SoundFont files",
	              fluid_synth_sfcount(synth.get()));

	// Create the mixer channel and move it into the member
	const auto mixer_callback = std::bind(&MidiHandlerFluidsynth::MixerCallBack,
	                                      this, std::placeholders::_1);
	mixer_channel_ptr_t mixer_channel(
	        MIXER_AddChannel(mixer_callback,
	                         static_cast<unsigned>(sample_rate), "FSYNTH"),
	        MIXER_DelChannel);
	channel = std::move(mixer_channel);

	// Register our volume callback with the mixer
	const auto set_mixer_volume = std::bind(&MidiHandlerFluidsynth::SetMixerVolume,
	                                        this, std::placeholders::_1);
	channel->RegisterVolCallBack(set_mixer_volume);

	channel->Enable(true);
	is_open = true;
	return true;
}

void MidiHandlerFluidsynth::Close()
{
	if (!is_open)
		return;

	channel->Enable(false);
	channel = nullptr;
	synth = nullptr;
	settings = nullptr;
	is_open = false;
}

void MidiHandlerFluidsynth::PlayMsg(const uint8_t *msg)
{
	const int chanID = msg[0] & 0b1111;

	switch (msg[0] & 0b1111'0000) {
	case 0b1000'0000:
		fluid_synth_noteoff(synth.get(), chanID, msg[1]);
		break;
	case 0b1001'0000:
		fluid_synth_noteon(synth.get(), chanID, msg[1], msg[2]);
		break;
	case 0b1010'0000:
		fluid_synth_key_pressure(synth.get(), chanID, msg[1], msg[2]);
		break;
	case 0b1011'0000:
		fluid_synth_cc(synth.get(), chanID, msg[1], msg[2]);
		break;
	case 0b1100'0000:
		fluid_synth_program_change(synth.get(), chanID, msg[1]);
		break;
	case 0b1101'0000:
		fluid_synth_channel_pressure(synth.get(), chanID, msg[1]);
		break;
	case 0b1110'0000:
		fluid_synth_pitch_bend(synth.get(), chanID, msg[1] + (msg[2] << 7));
		break;
	default: {
		uint64_t tmp;
		memcpy(&tmp, msg, sizeof(tmp));
		LOG_MSG("MIDI: unknown MIDI command: %0" PRIx64, tmp);
		break;
	}
	}
}

void MidiHandlerFluidsynth::PlaySysex(uint8_t *sysex, size_t len)
{
	const char *data = reinterpret_cast<const char *>(sysex);
	const auto n = static_cast<int>(len);
	fluid_synth_sysex(synth.get(), data, n, nullptr, nullptr, nullptr, false);
}

void MidiHandlerFluidsynth::PrintStats()
{
	soft_limiter.PrintStats(mixer_volume);
}

void MidiHandlerFluidsynth::MixerCallBack(uint16_t frames)
{
	constexpr uint16_t max_samples = expected_max_frames * 2; // two channels per frame
	std::array<float, max_samples> accumulator = {{0}};
	std::array<int16_t, max_samples> scaled = {{0}};

	while (frames > 0) {
		constexpr uint16_t max_frames = expected_max_frames; // local copy fixes link error
		const uint16_t len = std::min(frames, max_frames);
		fluid_synth_write_float(synth.get(), len, accumulator.data(), 0,
		                        2, accumulator.data(), 1, 2);
		soft_limiter.Apply(accumulator, scaled, len);
		channel->AddSamples_s16(len, scaled.data());
		frames -= len;
	}
}

static void fluid_destroy(MAYBE_UNUSED Section *sec)
{
	instance.PrintStats();
}

static void fluid_init(Section *sec)
{
	sec->AddDestroyFunction(&fluid_destroy, true);
}

void FLUID_AddConfigSection(Config *conf)
{
	assert(conf);
	Section_prop *sec = conf->AddSection_prop("fluidsynth", &fluid_init);
	assert(sec);
	init_fluid_dosbox_settings(*sec);
}

#endif // C_FLUIDSYNTH
