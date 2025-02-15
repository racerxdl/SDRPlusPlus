#pragma once
#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <config.h>
#include <dsp/chain.h>
#include <dsp/noise_reduction.h>

ConfigManager config;

#define CONCAT(a, b)    ((std::string(a) + b).c_str())

const double DeemphasisModes[] {
    50e-6,
    75e-6
};

const char* DeemhasisModesTxt = "50µS\00075µS\000None\000";

class NewRadioModule;
#include "demod.h"

class NewRadioModule : public ModuleManager::Instance {
public:
    NewRadioModule(std::string name) {
        this->name = name;

        // Initialize the config if it doesn't exist
        bool created = false;
        config.acquire();
        if (!config.conf.contains(name)) {
            config.conf[name]["selectedDemodId"] = 1;
            created = true;
        }
        selectedDemodID = config.conf[name]["selectedDemodId"];
        config.release(created);

        // Create demodulator instances
        demods.fill(NULL);
        demods[RADIO_DEMOD_WFM] = new demod::WFM();
        demods[RADIO_DEMOD_NFM] = new demod::NFM();
        demods[RADIO_DEMOD_AM] = new demod::AM();
        demods[RADIO_DEMOD_USB] = new demod::USB();
        demods[RADIO_DEMOD_LSB] = new demod::LSB();
        demods[RADIO_DEMOD_DSB] = new demod::DSB();
        demods[RADIO_DEMOD_CW] = new demod::CW();
        demods[RADIO_DEMOD_RAW] = new demod::RAW();

        // Initialize the VFO
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, 200000, 200000, 50000, 200000, false);
        onUserChangedBandwidthHandler.handler = vfoUserChangedBandwidthHandler;
        onUserChangedBandwidthHandler.ctx = this;
        vfo->wtfVFO->onUserChangedBandwidth.bindHandler(&onUserChangedBandwidthHandler);

        // Initialize IF DSP chain
        ifChainOutputChanged.ctx = this;
        ifChainOutputChanged.handler = ifChainOutputChangeHandler;
        ifChain.init(vfo->output, &ifChainOutputChanged);

        squelch.block.init(NULL, MIN_SQUELCH);

        ifChain.add(&squelch);

        // Load configuration for and enabled all demodulators
        EventHandler<dsp::stream<dsp::stereo_t>*> _demodOutputChangeHandler;
        _demodOutputChangeHandler.handler = demodOutputChangeHandler;
        _demodOutputChangeHandler.ctx = this;
        for (auto& demod : demods) {
            if (!demod) { continue; }

            // Default config
            double bw = demod->getDefaultBandwidth();
            if (!config.conf[name].contains(demod->getName())) {
                config.conf[name][demod->getName()]["bandwidth"] = bw;
                config.conf[name][demod->getName()]["snapInterval"] = demod->getDefaultSnapInterval();
                config.conf[name][demod->getName()]["squelchLevel"] = MIN_SQUELCH;
                config.conf[name][demod->getName()]["squelchEnabled"] = false;
                config.conf[name][demod->getName()]["deempMode"] = demod->getDefaultDeemphasisMode();
            }
            bw = std::clamp<double>(bw, demod->getMinBandwidth(), demod->getMaxBandwidth());
            
            // Initialize
            demod->init(name, &config, ifChain.getOutput(), bw, _demodOutputChangeHandler, stream.getSampleRate());
        }
        
        // Initialize audio DSP chain
        afChainOutputChanged.ctx = this;
        afChainOutputChanged.handler = afChainOutputChangeHandler;
        afChain.init(&dummyAudioStream, &afChainOutputChanged);

        win.init(24000, 24000, 48000);
        resamp.block.init(NULL, &win, 250000, 48000);
        deemp.block.init(NULL, 48000, 50e-6);
        deemp.block.bypass = false;

        afChain.add(&resamp);
        afChain.add(&deemp);

        // Initialize the sink
        srChangeHandler.ctx = this;
        srChangeHandler.handler = sampleRateChangeHandler;
        stream.init(afChain.getOutput(), &srChangeHandler, audioSampleRate);
        sigpath::sinkManager.registerStream(name, &stream);

        // Select the demodulator
        selectDemodByID((DemodID)selectedDemodID);

        // Start IF chain
        ifChain.start();

        // Start AF chain
        afChain.start();

        // Start stream, the rest was started when selecting the demodulator
        stream.start();

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~NewRadioModule() {
        gui::menu.removeEntry(name);
        stream.stop();
        if (enabled) {
            disable();
        }
        sigpath::sinkManager.unregisterStream(name);
    }

    void postInit() {}

    void enable() {
        enabled = true;
        if (!vfo) {
            vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, 200000, 200000, 50000, 200000, false);
            vfo->wtfVFO->onUserChangedBandwidth.bindHandler(&onUserChangedBandwidthHandler);
        }
        selectDemodByID((DemodID)selectedDemodID);
    }

    void disable() {
        enabled = false;
        ifChain.stop();
        if (selectedDemod) { selectedDemod->stop(); }
        afChain.stop();
        if (vfo) { sigpath::vfoManager.deleteVFO(vfo); }
        vfo = NULL;
    }

    bool isEnabled() {
        return enabled;
    }

    std::string name;

    enum DemodID {
        RADIO_DEMOD_NFM,
        RADIO_DEMOD_WFM,
        RADIO_DEMOD_AM,
        RADIO_DEMOD_DSB,
        RADIO_DEMOD_USB,
        RADIO_DEMOD_CW,
        RADIO_DEMOD_LSB,
        RADIO_DEMOD_RAW,
        _RADIO_DEMOD_COUNT,
    };

private:
    static void menuHandler(void* ctx) {
        NewRadioModule* _this = (NewRadioModule*)ctx;

        if (!_this->enabled) { style::beginDisabled(); }

        float menuWidth = ImGui::GetContentRegionAvailWidth();
        ImGui::BeginGroup();

        ImGui::Columns(4, CONCAT("RadioModeColumns##_", _this->name), false);
        if (ImGui::RadioButton(CONCAT("NFM##_", _this->name), _this->selectedDemodID == 0) && _this->selectedDemodID != 0) { 
            _this->selectDemodByID(RADIO_DEMOD_NFM);
        }
        if (ImGui::RadioButton(CONCAT("WFM##_", _this->name), _this->selectedDemodID == 1) && _this->selectedDemodID != 1) {
            _this->selectDemodByID(RADIO_DEMOD_WFM);
        }
        ImGui::NextColumn();
        if (ImGui::RadioButton(CONCAT("AM##_", _this->name), _this->selectedDemodID == 2) && _this->selectedDemodID != 2) {
            _this->selectDemodByID(RADIO_DEMOD_AM);
        }
        if (ImGui::RadioButton(CONCAT("DSB##_", _this->name), _this->selectedDemodID == 3) && _this->selectedDemodID != 3)  {
            _this->selectDemodByID(RADIO_DEMOD_DSB);
        }
        ImGui::NextColumn();
        if (ImGui::RadioButton(CONCAT("USB##_", _this->name), _this->selectedDemodID == 4) && _this->selectedDemodID != 4) {
            _this->selectDemodByID(RADIO_DEMOD_USB);
        }
        if (ImGui::RadioButton(CONCAT("CW##_", _this->name), _this->selectedDemodID == 5) && _this->selectedDemodID != 5) {
            _this->selectDemodByID(RADIO_DEMOD_CW);
        };
        ImGui::NextColumn();
        if (ImGui::RadioButton(CONCAT("LSB##_", _this->name), _this->selectedDemodID == 6) && _this->selectedDemodID != 6) {
            _this->selectDemodByID(RADIO_DEMOD_LSB);
        }
        if (ImGui::RadioButton(CONCAT("RAW##_", _this->name), _this->selectedDemodID == 7) && _this->selectedDemodID != 7) {
            _this->selectDemodByID(RADIO_DEMOD_RAW);
        };
        ImGui::Columns(1, CONCAT("EndRadioModeColumns##_", _this->name), false);

        ImGui::EndGroup();

        if (!_this->bandwidthLocked) {
            ImGui::LeftLabel("Bandwidth");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputFloat(("##_radio_bw_" + _this->name).c_str(), &_this->bandwidth, 1, 100, "%.0f")) {
                _this->bandwidth = std::clamp<float>(_this->bandwidth, _this->minBandwidth, _this->maxBandwidth);
                _this->setBandwidth(_this->bandwidth);
                config.acquire();
                config.conf[_this->name][_this->selectedDemod->getName()]["bandwidth"] = _this->bandwidth;
                config.release(true);
            }
        }

        ImGui::LeftLabel("Snap Interval");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(("##_radio_snap_" + _this->name).c_str(), &_this->snapInterval, 1, 100)) {
            if (_this->snapInterval < 1) { _this->snapInterval = 1; }
            _this->vfo->setSnapInterval(_this->snapInterval);
            config.acquire();
            config.conf[_this->name][_this->selectedDemod->getName()]["snapInterval"] = _this->snapInterval;
            config.release(true);
        }

        if (_this->deempAllowed) {
            ImGui::LeftLabel("De-emphasis");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::Combo(("##_radio_wfm_deemp_" + _this->name).c_str(), &_this->deempMode, DeemhasisModesTxt)) {
                _this->setDeemphasisMode(_this->deempMode);
                config.acquire();
                config.conf[_this->name][_this->selectedDemod->getName()]["deempMode"] = _this->deempMode;
                config.release(true);
            }
        }

        if (ImGui::Checkbox(("Squelch##_radio_sqelch_ena_" + _this->name).c_str(), &_this->squelchEnabled)) {
            _this->setSquelchEnabled(_this->squelchEnabled);
            config.acquire();
            config.conf[_this->name][_this->selectedDemod->getName()]["squelchEnabled"] = _this->squelchEnabled;
            config.release(true);
        }
        if (!_this->squelchEnabled && _this->enabled) { style::beginDisabled(); }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::SliderFloat(("##_radio_sqelch_lvl_" + _this->name).c_str(), &_this->squelchLevel, _this->MIN_SQUELCH, _this->MAX_SQUELCH, "%.3fdB")) {
            _this->squelch.block.setLevel(_this->squelchLevel);
            config.acquire();
            config.conf[_this->name][_this->selectedDemod->getName()]["squelchLevel"] = _this->squelchLevel;
            config.release(true);
        }
        if (!_this->squelchEnabled && _this->enabled) { style::endDisabled(); }

        _this->selectedDemod->showMenu();

        if (!_this->enabled) { style::endDisabled(); }
    }

    void selectDemodByID(DemodID id) {
        demod::Demodulator* demod = demods[id];
        if (!demod) {
            spdlog::error("Demodulator {0} not implemented", id);
            return;
        }
        selectedDemodID = id;
        selectDemod(demod);

        // Save config
        config.acquire();
        config.conf[name]["selectedDemodId"] = id;
        config.release(true);
    }

    void selectDemod(demod::Demodulator* demod) {
        // Stopcurrently selected demodulator and select new
        if (selectedDemod) { selectedDemod->stop(); }
        selectedDemod = demod;

        // Give the demodulator the most recent audio SR
        selectedDemod->AFSampRateChanged(audioSampleRate);

        // Set the demodulator's input
        selectedDemod->setInput(ifChain.getOutput());

        // Set AF chain's input
        afChain.setInput(selectedDemod->getOutput());

        // Load config
        bandwidth = selectedDemod->getDefaultBandwidth();
        minBandwidth = selectedDemod->getMinBandwidth();
        maxBandwidth = selectedDemod->getMaxBandwidth();
        bandwidthLocked = selectedDemod->getBandwidthLocked();
        snapInterval = selectedDemod->getDefaultSnapInterval();
        squelchLevel = MIN_SQUELCH;
        deempAllowed = selectedDemod->getDeempAllowed();
        deempMode = DEEMP_MODE_NONE;
        squelchEnabled = false;
        postProcEnabled = selectedDemod->getPostProcEnabled();
        if (config.conf[name][selectedDemod->getName()].contains("snapInterval")) {
            bandwidth = config.conf[name][selectedDemod->getName()]["bandwidth"];
            bandwidth = std::clamp<double>(bandwidth, minBandwidth, maxBandwidth);
        }
        if (config.conf[name][selectedDemod->getName()].contains("snapInterval")) {
            snapInterval = config.conf[name][selectedDemod->getName()]["snapInterval"];
        }
        if (config.conf[name][selectedDemod->getName()].contains("squelchLevel")) {
            squelchLevel = config.conf[name][selectedDemod->getName()]["squelchLevel"];
        }
        if (config.conf[name][selectedDemod->getName()].contains("squelchEnabled")) {
            squelchEnabled = config.conf[name][selectedDemod->getName()]["squelchEnabled"];
        }
        if (config.conf[name][selectedDemod->getName()].contains("deempMode")) {
            deempMode = config.conf[name][selectedDemod->getName()]["deempMode"];
        }
        deempMode = std::clamp<int>(deempMode, 0, _DEEMP_MODE_COUNT-1);

        // Configure VFO
        if (vfo) {
            vfo->setBandwidthLimits(minBandwidth, maxBandwidth, selectedDemod->getBandwidthLocked());
            vfo->setReference(selectedDemod->getVFOReference());
            vfo->setSnapInterval(snapInterval);
            vfo->setSampleRate(selectedDemod->getIFSampleRate(), bandwidth);
        }

        // Configure IF chain
        squelch.block.setLevel(squelchLevel);
        setSquelchEnabled(squelchEnabled);

        // Configure AF chain
        if (postProcEnabled) {
            // Configure resampler
            afChain.stop();
            resamp.block.setInSampleRate(selectedDemod->getAFSampleRate());
            setAudioSampleRate(audioSampleRate);
            afChain.enable(&resamp);

            // Configure deemphasis
            setDeemphasisMode(deempMode);
        }
        else {
            // Disable everyting if post processing is disabled
            afChain.disableAll();
        }

        // Start new demodulator
        selectedDemod->start();
    }


    void setBandwidth(double bw) {
        bandwidth = bw;
        if (!selectedDemod) { return; }
        float audioBW = std::min<float>(selectedDemod->getMaxAFBandwidth(), selectedDemod->getAFBandwidth(bandwidth));
        audioBW = std::min<float>(audioBW, audioSampleRate / 2.0);
        vfo->setBandwidth(bandwidth);
        selectedDemod->setBandwidth(bandwidth);

        // Only bother with setting the resampling setting if we're actually post processing and dynamic bw is enabled
        if (selectedDemod->getDynamicAFBandwidth() && postProcEnabled) {
            win.setCutoff(audioBW);
            win.setTransWidth(audioBW);
            resamp.block.updateWindow(&win);
        }

        config.acquire();
        config.conf[name][selectedDemod->getName()]["bandwidth"] = bandwidth;
        config.release(true);
    }

    void setAudioSampleRate(double sr) {
        audioSampleRate = sr;
        if (!selectedDemod) { return; }
        selectedDemod->AFSampRateChanged(audioSampleRate);
        if (!postProcEnabled) {
            // If postproc is disabled, IF SR = AF SR
            minBandwidth = selectedDemod->getMinBandwidth();
            maxBandwidth = selectedDemod->getMaxBandwidth();
            bandwidth = selectedDemod->getIFSampleRate();
            vfo->setBandwidthLimits(minBandwidth, maxBandwidth, selectedDemod->getBandwidthLocked());
            vfo->setSampleRate(selectedDemod->getIFSampleRate(), bandwidth);
            return;
        }
        float audioBW = std::min<float>(selectedDemod->getMaxAFBandwidth(), selectedDemod->getAFBandwidth(bandwidth));
        audioBW = std::min<float>(audioBW, audioSampleRate / 2.0);

        afChain.stop();

        // Configure resampler
        resamp.block.setOutSampleRate(audioSampleRate);
        win.setSampleRate(selectedDemod->getAFSampleRate() * resamp.block.getInterpolation());
        win.setCutoff(audioBW);
        win.setTransWidth(audioBW);
        resamp.block.updateWindow(&win);

        // Configure deemphasis sample rate
        deemp.block.setSampleRate(audioSampleRate);
        
        afChain.start();
    }

    void setDeemphasisMode(int mode) {
        deempMode = mode;
        if (!postProcEnabled) { return; }
        bool deempEnabled = (deempMode != DEEMP_MODE_NONE);
        if (deempEnabled) {
            deemp.block.setTau(DeemphasisModes[deempMode]);
        }
        afChain.setState(&deemp, deempEnabled);
    }

    void setSquelchEnabled(bool enable) {
        squelchEnabled = enable;
        if (!selectedDemod) { return; }
        ifChain.setState(&squelch, squelchEnabled);
    }

    static void vfoUserChangedBandwidthHandler(double newBw, void* ctx) {
        NewRadioModule* _this = (NewRadioModule*)ctx;
        _this->setBandwidth(newBw);
    }

    static void sampleRateChangeHandler(float sampleRate, void* ctx) {
        NewRadioModule* _this = (NewRadioModule*)ctx;
        _this->setAudioSampleRate(sampleRate);
    }

    static void demodOutputChangeHandler(dsp::stream<dsp::stereo_t>* output, void* ctx) {
        NewRadioModule* _this = (NewRadioModule*)ctx;
        _this->afChain.setInput(output);
    }

    static void ifChainOutputChangeHandler(dsp::stream<dsp::complex_t>* output, void* ctx) {
        NewRadioModule* _this = (NewRadioModule*)ctx;
        if (!_this->selectedDemod) { return; }
        _this->selectedDemod->setInput(output);
    }

    static void afChainOutputChangeHandler(dsp::stream<dsp::stereo_t>* output, void* ctx) {
        NewRadioModule* _this = (NewRadioModule*)ctx;
        _this->stream.setInput(output);
    }
    
    // Handlers
    EventHandler<double> onUserChangedBandwidthHandler;
    EventHandler<float> srChangeHandler;
    EventHandler<dsp::stream<dsp::complex_t>*> ifChainOutputChanged;
    EventHandler<dsp::stream<dsp::stereo_t>*> afChainOutputChanged;

    VFOManager::VFO* vfo = NULL;

    // IF chain
    dsp::Chain<dsp::complex_t> ifChain;
    dsp::ChainLink<dsp::Squelch, dsp::complex_t> squelch;

    // Audio chain
    dsp::stream<dsp::stereo_t> dummyAudioStream;
    dsp::Chain<dsp::stereo_t> afChain;
    dsp::filter_window::BlackmanWindow win;
    dsp::ChainLink<dsp::PolyphaseResampler<dsp::stereo_t>, dsp::stereo_t> resamp;
    dsp::ChainLink<dsp::BFMDeemp, dsp::stereo_t> deemp;

    SinkManager::Stream stream;

    std::array<demod::Demodulator*, _RADIO_DEMOD_COUNT> demods;
    demod::Demodulator* selectedDemod = NULL;

    double audioSampleRate = 48000.0;
    float minBandwidth;
    float maxBandwidth;
    float bandwidth;
    bool bandwidthLocked;
    int snapInterval;
    bool squelchEnabled = false;
    float squelchLevel;
    int selectedDemodID = 1;
    int deempMode = DEEMP_MODE_NONE;
    bool deempAllowed;
    bool postProcEnabled;

    const double MIN_SQUELCH = -100.0;
    const double MAX_SQUELCH = 0.0;

    bool enabled = true;

};