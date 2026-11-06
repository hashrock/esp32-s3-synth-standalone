#include "dsp/rhodes.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

void wav(const std::string &path, const std::vector<float> &samples, int sr) {
    std::ofstream out(path, std::ios::binary);
    auto u16 = [&](uint16_t v) { out.put(v & 255); out.put(v >> 8); };
    auto u32 = [&](uint32_t v) { u16(v & 65535); u16(v >> 16); };
    out.write("RIFF", 4); u32(36 + samples.size()*2); out.write("WAVEfmt ", 8);
    u32(16); u16(1); u16(1); u32(sr); u32(sr*2); u16(2); u16(16);
    out.write("data", 4); u32(samples.size()*2);
    for (float s : samples) u16(static_cast<int16_t>(std::clamp(s,-1.f,1.f) * 32767));
    assert(out.good());
}
int main(int argc, char **argv) {
    const std::string dir = argc > 1 ? argv[1] : "/tmp";
    rhodes::Bank<float> bank; bank.init(48000, 4, true);
    float sqrt_error = 0;
    for (int i=0; i<10000; ++i) {
        const float x = std::exp(-45.f + i*.0045f);
        sqrt_error = std::max(sqrt_error, std::abs(rhodes::contact_sqrt(x)/std::sqrt(x)-1));
    }
    assert(sqrt_error < 3e-7f);
    std::printf("contact sqrt relative error %.9g\n", sqrt_error);
    float max_peak = 0; unsigned max_handover = 0;
    for (int n = 0; n < 128; ++n) for (int vel : {1, 32, 78, 127}) {
        rhodes::Voice a, b;
        a.note_on(bank, n, vel); b.note_on(bank, n, vel);
        for (int i = 0; i < 48000; ++i) {
            if (i == 6000) { a.note_off(); b.note_off(); }
            const float x = a.process();
            assert(std::isfinite(x) && std::abs(x) < .62f);
            assert(x == b.process());
            max_peak = std::max(max_peak, std::abs(x));
        }
        assert(a.idle());
        if (n < 125) assert(a.handover_frame() > 0);
        max_handover = std::max(max_handover, a.handover_frame());
    }
    std::printf("128 notes x 4 velocities: finite, bounded, repeatable, released; peak %.6f, latest handover %.3f ms\n", max_peak, max_handover/48.0);
    for (int note : {0,24,60,84,100,124}) {
        rhodes::Voice block, scalar;
        block.note_on(bank,note,127); scalar.note_on(bank,note,127);
        float tail=.1f, last=0, reference_tail=.1f;
        for (int b=0; b<800; ++b) {
            float mix[64]; for (float &x:mix) x=.02f;
            if (b==5) { block.note_off(); scalar.note_off(); }
            block.render_mixed(mix,64,1.25f,tail,last);
            for (float x:mix) {
                const float expected=(scalar.process()+reference_tail)*1.25f+.02f;
                reference_tail*=.9862071f;
                assert(std::abs(x-expected)<1e-6f);
            }
        }
    }
    std::puts("block vs scalar rendering including release/tail: passed");
    rhodes::Voice held; held.note_on(bank, 60, 127);
    for (int i=0; i<48000*17; ++i) assert(std::isfinite(held.process()));
    assert(held.idle() && held.process() == 0);
    for (const auto &settings : {rhodes::kBark, rhodes::kVelvet}) {
        rhodes::Bank<float> variant; variant.init(48000, 4, true, settings);
        float peak=0;
        if (settings.velocity_timbre) {
            assert(variant.tones[20].alpha < variant.tones[78].alpha);
            assert(variant.tones[78].alpha < variant.tones[127].alpha);
            assert(!settings.saturate_output || variant.tones[20].drive < variant.tones[127].drive);
            assert(variant.tones[20].pickup_position_scale < variant.tones[127].pickup_position_scale);
        }
        if (settings.pickup_mix) {
            const double offset=settings.pickup_offset/settings.pickup_gap;
            const double nominal=-2*offset/std::pow(1+offset*offset,2);
            double error=0;
            for (int i=0; i<10000; ++i) {
                const float x=-.008f+i*.016f/9999;
                const double t=(x-settings.pickup_offset)/settings.pickup_gap;
                const double factor=std::clamp(2*t/std::pow(1+t*t,2)/nominal,-3.0,3.0);
                const double exact=1-settings.pickup_mix+settings.pickup_mix*factor;
                error=std::max(error,std::abs(variant.pickup_gain(x)-exact));
            }
            // Clamped pickup corners move with offset; allow 0.5% of the
            // full gain span (factor -3..3), including interpolation at corners.
            assert(error < .005 * 6 * settings.pickup_mix);
            assert(std::abs(variant.pickup_gain(0)-1)<1e-6f);
            std::printf("pickup LUT max error %.6f\n",error);
            rhodes::Voice block,scalar; block.note_on(variant,60,127);scalar.note_on(variant,60,127);
            float tail=0,last=0;
            for(int b=0;b<1000;++b) {
                float mix[64]{};
                if(b==10){block.note_off();scalar.note_off();}
                block.render_mixed(mix,64,1,tail,last);
                for(float x:mix) assert(std::abs(x-scalar.process())<1e-6f);
            }
        }
        for (int n=0; n<128; ++n) for (int vel : {1,78,127}) {
            rhodes::Voice voice; voice.note_on(variant,n,vel);
            for (int i=0; i<72000; ++i) {
                if (i==4800) voice.note_off();
                float x=voice.process();
                assert(std::isfinite(x) && std::abs(x)<(settings.saturate_output ? .62f : 4.f));
                peak=std::max(peak,std::abs(x));
            }
            assert(voice.idle());
        }
        std::printf("variant cutoff %.0f: all notes stable/released, peak %.6f\n", settings.cutoff, peak);
        rhodes::Voice voice; voice.note_on(variant,60,100);
        std::vector<float> clip(48000*4);
        for(size_t i=0; i<clip.size(); ++i) {
            if(i==48000*2) voice.note_off();
            clip[i]=voice.process();
            if(settings.cutoff<2000) clip[i]*=.7f+.3f*std::sin(2*rhodes::Bank<float>::pi*4.2f*i/48000);
        }
        wav(dir + (settings.cutoff<2000 ? "/rhodes-velvet.wav" : "/rhodes-bark.wav"),clip,48000);
    }
    // Double precision full Euler versus production float, same 44.1 kHz as JS.
    rhodes::Bank<double> reference; reference.init(44100, 4);
    rhodes::Bank<float> comparison; comparison.init(44100, 4);
    rhodes::BasicVoice<double> ref;
    rhodes::Voice single;
    ref.note_on(reference, 60, 78, false); single.note_on(comparison, 60, 78, false);
    std::vector<float> golden(185220);
    double error = 0;
    for (auto &x : golden) { x = ref.process(); error = std::max(error, std::abs(double(x-single.process()))); }
    // Precomputed Euler coefficients change float rounding; < -87 dBFS error.
    assert(error < 4e-5);
    std::ofstream raw(dir + "/rhodes-reference.f32", std::ios::binary);
    raw.write(reinterpret_cast<const char *>(golden.data()), golden.size()*sizeof(float)); raw.close();
    std::printf("float vs double full Euler max error %.9g\n", error);
    wav(dir + "/rhodes-reference-44100.wav", golden, 44100);
    for (int n : {36, 60, 84}) {
        std::vector<float> clip(48000*5);
        rhodes::Voice v; v.note_on(bank, n, 78);
        double early = 0, late = 0;
        for (size_t i = 0; i < clip.size(); ++i) {
            if (i == 48000*4) v.note_off();
            clip[i] = v.process();
            if (i < 48000) early += clip[i]*clip[i];
            if (i >= 48000*3 && i < 48000*4) late += clip[i]*clip[i];
        }
        assert(late < early*.02);
        assert(v.idle());
        wav(dir + "/rhodes-" + std::to_string(n) + ".wav", clip, 48000);
    }
    // Convergence of attack (first 80ms); free motion has intentionally different
    // dispersion from Euler, so compare full integration at each oversampling.
    for (int n : {36, 60, 84}) {
        std::vector<double> signal[3];
        for (int k = 0; k < 3; ++k) {
            rhodes::Bank<double> b; b.init(48000, 2 << k);
            rhodes::BasicVoice<double> v; v.note_on(b, n, 127, false);
            for (int i = 0; i < 3840; ++i) signal[k].push_back(v.process());
        }
        double e24=0, e48=0;
        for (int i=0;i<3840;++i) {
            e24 += std::pow(signal[0][i]-signal[1][i],2);
            e48 += std::pow(signal[1][i]-signal[2][i],2);
        }
        assert(e48 < e24);
        std::printf("note %d attack convergence RMS 2x/4x %.7f, 4x/8x %.7f\n", n, std::sqrt(e24/3840), std::sqrt(e48/3840));
    }
    // On the exact handover sample output and q/v still match the Euler path.
    for (int n : {0, 36, 60, 84, 124}) {
        rhodes::Voice opt, full; opt.note_on(bank,n,127); full.note_on(bank,n,127,false);
        unsigned contacts = 0;
        for (int i=0;i<24000;++i) {
            float x=opt.process(), y=full.process();
            if (!opt.handover_frame() || unsigned(i+1)==opt.handover_frame()) assert(x==y);
            if (unsigned(i+1)==opt.handover_frame()) contacts=full.contact_samples();
        }
        assert(contacts == full.contact_samples());
    }
    std::puts("handover continuity and no later contacts: passed");
}
