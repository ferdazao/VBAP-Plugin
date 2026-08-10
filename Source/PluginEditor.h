#pragma once
#include <JuceHeader.h>
#include <optional>
#include "PluginProcessor.h"

class VBAPAudioProcessorEditor : public juce::AudioProcessorEditor,
    private juce::Timer
{
public:
    static constexpr int kEditorW = 1180;
    static constexpr int kEditorH = 700;

    explicit VBAPAudioProcessorEditor(VBAPAudioProcessor&);
    ~VBAPAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;

    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    enum class CamPreset { Iso, Top, Front, Side };
    void applyPreset(CamPreset);
    void updatePresetButtonStates() noexcept;
    std::optional<CamPreset> activePreset = CamPreset::Iso;

    VBAPAudioProcessor& audioProcessor;

    // ---- Sliders esfericos (sin attachment APVTS) ----
    juce::Slider azSlider;
    juce::Slider elSlider;
    juce::Slider distSlider;

    // ---- Downmix + Swap + Presets ----
    juce::ToggleButton downmixButton;
    juce::ToggleButton swapLRButton;
    juce::TextButton   resetCamButton;
    juce::TextButton   presetIsoBtn;
    juce::TextButton   presetTopBtn;
    juce::TextButton   presetFrontBtn;
    juce::TextButton   presetSideBtn;
    juce::ToggleButton delayCButton;

    // ---- APVTS Attachments ----
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> delayCEnabledAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> downmixAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> swapLRAtt;

    // ---- Punteros atomicos cacheados (sin hash lookups en path caliente) ----
    std::atomic<float>* pSrcX    = nullptr;
    std::atomic<float>* pSrcY    = nullptr;
    std::atomic<float>* pSrcZ    = nullptr;
    std::atomic<float>* pSwapLR  = nullptr;

    // ---- Areas de layout ----
    juce::Rectangle<int> isoArea;
    juce::Rectangle<int> gainArea;

    // ---- Estado de audio ----
    float gains[3] = { 0.33f, 0.33f, 0.33f };
    bool  updatingFromTimer = false;

    // ---- Camara orbital ----
    float camAzimuth = 45.0f;
    float camElevation = 30.0f;
    float camScale = 70.0f;    // alineado con CAM_SCALE_DEFAULT en .cpp

    // Cache de trig de camara (refrescar via updateCameraTrig() cuando
    // camAzimuth o camElevation cambien). Evita sin/cos por punto proyectado.
    float camCosAz = 1.0f;
    float camSinAz = 0.0f;
    float camSinEl = 0.5f;   // sin(30°)
    float camCosEl = 0.866f; // cos(30°)
    void  updateCameraTrig() noexcept;

    // Para repaint condicional en timerCallback.
    float prevX = -1e9f, prevY = -1e9f, prevZ = -1e9f;
    float prevGains[3] { -1e9f, -1e9f, -1e9f };

    static constexpr float CAM_AZ_DEFAULT = 45.0f;
    static constexpr float CAM_EL_DEFAULT = 30.0f;
    static constexpr float CAM_AZ_SENS = 0.40f;
    static constexpr float CAM_EL_SENS = 0.25f;

    // ---- Estado del mouse ----
    enum class DragMode { None, MoveSrc, RotateCamera };
    DragMode           dragMode = DragMode::None;
    bool               hovering = false;
    bool               zDragActive = false;
    juce::Point<float> lastDragPos;
    juce::Point<float> dragCursorPos;
    int                hoverSpeakerIdx = -1;
    int                dominantSpeakerIdx = -1;

    static constexpr float GRAB_R = 20.0f;

    // ---- Dimensiones del cuarto (X/Z ~3m, Y ampliado a 6m para dejar profundidad detras del plano de altavoces) ----
    static constexpr float ROOM_X = 3.0f;
    static constexpr float ROOM_Y = 6.0f;   // profundidad ampliada (back-of-plane)
    static constexpr float ROOM_Z = 3.0f;

    // ---- Mapeo lineal desacoplado (NO esferico) ----
    // Az [-AZ_MAX..AZ_MAX] deg -> X lineal en [-SOURCE_X_MAX, SOURCE_X_MAX]
    // El [0..EL_MAX]       deg -> Z lineal en [kListenerZ, ROOM_Z]
    // Dist [2.42..6] m         -> Y mundo (linealmente; controla profundidad).
    //
    // AZ_MAX = 84° se calcula para que la escala m/grado coincida con la
    // original (1.40 / 84 = 1/60 m/deg). El slider queda limitado a la
    // posicion lateral del altavoz L/R; mas alla VBAP saca a la fuente del
    // triangulo y las ganancias se hacen confusas.
    //
    // La distancia euclidea real al sweet spot se muestra en el readout
    // inferior (drawAxesAndOverlay), no en el slider en si.
    static constexpr float SOURCE_X_MAX = VBAPAudioProcessor::kSpeakerXAbs;  // 1.40 m
    static constexpr float SOURCE_Z_MAX = VBAPAudioProcessor::kSpeakerZMax;  // 2.00 m (= altura del C)
    static constexpr float AZ_MAX   = 84.0f;
    static constexpr float EL_MAX   = 90.0f;
    static constexpr float DIST_MIN =  2.42f;   // sweet-spot Y (= kYSpeaker)
    static constexpr float DIST_MAX =  6.0f;    // pared trasera

    juce::Point<float> isoScreenCenter;

    // ---- Proyeccion orbital ----
    juce::Point<float>     isoProj(float wx, float wy, float wz) const;
    std::pair<float, float> isoInverseXY(juce::Point<float> sp, float wz) const;

    // ---- Conversiones esfericas ----
    void sphericalToXYZ(float az, float el, float dist, float& x, float& y, float& z) const;
    void xyzToSpherical(float x, float y, float z, float& az, float& el, float& dist) const;
    void onSphericalSliderChanged();
    void setXYZParameters(float x, float y, float z);

    // ---- Dibujo ----
    void drawRoom3D(juce::Graphics&);
    void drawFloor(juce::Graphics&);
    void drawWalls(juce::Graphics&);
    void drawCeilingEdges(juce::Graphics&);
    void drawGrids(juce::Graphics&);
    void drawDistanceRings(juce::Graphics&);
    void drawEnergyLines(juce::Graphics&, juce::Point<float> srcPt,
        const juce::Point<float> spkPts[3]);
    void drawSpeaker3D(juce::Graphics&, juce::Point<float> pos,
        const juce::String& label, float gainVal,
        bool dominant, bool mouseHovered);
    void drawSourcePoint(juce::Graphics&, juce::Point<float> srcPt,
        float sx, float sy, float sz);
    void drawAxesAndOverlay(juce::Graphics&);
    void drawCameraHint(juce::Graphics&);
    void drawGainBars(juce::Graphics&);
    void fillQuad(juce::Graphics&,
        juce::Point<float> a, juce::Point<float> b,
        juce::Point<float> c, juce::Point<float> d,
        juce::Colour col);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VBAPAudioProcessorEditor)
};