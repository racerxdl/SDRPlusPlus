#pragma once
#include "../demod.h"
#include <dsp/demodulator.h>
#include <dsp/filter.h>

namespace demod {
    class CW : public Demodulator {
    public:
        CW() {}
    
        CW(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, EventHandler<dsp::stream<dsp::stereo_t>*> outputChangeHandler, double audioSR) {
            init(name, config, input, bandwidth, outputChangeHandler, audioSR);
        }

        ~CW() {
            stop();
        }

        void init(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, EventHandler<dsp::stream<dsp::stereo_t>*> outputChangeHandler, double audioSR) {
            this->name = name;
            this->outputChangeHandler = outputChangeHandler;

            // Define structure
            xlator.init(input, getIFSampleRate(), 1000.0);
            c2r.init(&xlator.out);
            agc.init(&c2r.out, 20.0f, getIFSampleRate());
            m2s.init(&agc.out);
        }

        void start() {
            xlator.start();
            c2r.start();
            agc.start();
            m2s.start();
        }

        void stop() {
            xlator.stop();
            c2r.stop();
            agc.stop();
            m2s.stop();
        }

        void showMenu() {}

        void setBandwidth(double bandwidth) {}

        void setInput(dsp::stream<dsp::complex_t>* input) {
            xlator.setInput(input);
        }

        void AFSampRateChanged(double newSR) {}

        // ============= INFO =============

        const char* getName()                   { return "CW"; }
        double getIFSampleRate()                { return 3000.0; }
        double getAFSampleRate()                { return getIFSampleRate(); }
        double getDefaultBandwidth()            { return 500.0; }
        double getMinBandwidth()                { return 50.0; }
        double getMaxBandwidth()                { return 500.0; }
        bool getBandwidthLocked()               { return false; }
        double getMaxAFBandwidth()              { return getIFSampleRate() / 2.0; }
        double getDefaultSnapInterval()         { return 10.0; }
        int getVFOReference()                   { return ImGui::WaterfallVFO::REF_CENTER; }
        bool getDeempAllowed()                  { return false; }
        bool getPostProcEnabled()               { return true; }
        int getDefaultDeemphasisMode()          { return DEEMP_MODE_NONE; }
        double getAFBandwidth(double bandwidth) { return (bandwidth / 2.0) + 1000.0; }
        bool getDynamicAFBandwidth()            { return true; }
        dsp::stream<dsp::stereo_t>* getOutput() { return &m2s.out; }

    private:
        dsp::FrequencyXlator<dsp::complex_t> xlator;
        dsp::ComplexToReal c2r;
        dsp::AGC agc;
        dsp::MonoToStereo m2s;

        std::string name;
        EventHandler<dsp::stream<dsp::stereo_t>*> outputChangeHandler;
    };
}