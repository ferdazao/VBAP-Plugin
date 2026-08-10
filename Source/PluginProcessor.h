// PluginProcessor.h  -  VBAP Spatializer (mono input -> 3 discrete outputs)

#pragma once
#include <JuceHeader.h>
#include <Eigen/Dense>
#include <array>
#include "VBAPProcessor.h"

class VBAPAudioProcessor : public juce::AudioProcessor
{
public:
    static constexpr int   kNumSpeakers = 3;
    static constexpr float kMaxDelayMs  = 100.0f;

    // Delay fijo de alineacion del Center cuando el toggle "delayCEnabled" esta
    // activo. El valor corresponde a la diferencia de distancia entre C (~2.55 m
    // del oyente) y L/R (~2.80 m): 0.25 m / 343 m/s ~= 0.73 ms. Antes era un
    // slider 0-50 ms, ahora es un toggle on/off porque la geometria es fija y
    // el usuario no necesita variarlo.
    static constexpr float kCenterDelayMs = 0.73f;

    // Altura del oido del oyente (m). Posicion del oyente = (0, 0, kListenerZ).
    // VBAP resta este offset a la fuente antes de calcular ganancias, lo que
    // mueve el origen efectivo del oyente al nivel auditivo (ear level).
    static constexpr float kListenerZ   = 1.20f;

    // Atenuacion por distancia (curva 1/r reescalada referenciada al sweet spot).
    // Floor: gain plana a kMinGainDb cuando D >= kMaxDistance.
    // kRefDistance: tope de 0 dB cuando D <= 1 m (evita ganancia explosiva en D->0).
    // kMaxDistance: distancia a la que la curva toca exactamente kMinGainDb. Se
    // eligio 8 m: con el slider de profundidad al fondo (sourceY=6, D~3.6 m) la
    // ganancia se sienta ~-15 dB, dejando rango visible significativo en las
    // barras (~9 dB de variacion entre default y back wall).
    static constexpr float kMinGainDb   = -24.0f;
    static constexpr float kRefDistance =   1.0f;
    static constexpr float kMaxDistance =   8.0f;

    // Posicion X absoluta de los altavoces L y R (m). El rango de sourceX y el
    // slider de azimut se limitan a este valor: la fuente no debe salir
    // lateralmente del triangulo de altavoces, porque mas alla VBAP da
    // ganancias decrecientes que el usuario percibe como "se apaga el sonido"
    // sin razon aparente. Si cambias las posiciones L/R en prepareToPlay,
    // actualiza tambien este valor.
    static constexpr float kSpeakerXAbs = 1.40f;

    // Altura Z maxima del triangulo de altavoces (m), = posicion del Center
    // (el mas alto). El rango de sourceZ y el slider de elevacion se limitan
    // a este valor: analogo a kSpeakerXAbs en X, evita que la fuente salga
    // del triangulo por arriba (donde VBAP empezaria a clampear ganancias).
    // El limite inferior se mantiene en kListenerZ. Si cambias la altura del
    // Center en prepareToPlay, actualiza tambien este valor.
    static constexpr float kSpeakerZMax = 2.00f;

    VBAPAudioProcessor();
    ~VBAPAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts; }
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    float getLastGain(int ch) const noexcept
    {
        if (ch >= 0 && ch < 3) return lastGains[ch].load();
        return 0.0f;
    }
    const Eigen::Matrix<float, 3, 3>& getSpeakerPositions() const noexcept { return altPosRows; }

    // Sweet spot = midpoint(L, R) en coordenadas de mundo. Se recalcula en
    // prepareToPlay siempre que cambien las posiciones de altavoz. Es el
    // origen del escalar de distancia que alimenta la atenuacion y el slider
    // "Distancia [m]". NO es el origen del marco VBAP (ese sigue siendo el
    // oyente, ver nota en processBlock).
    const Eigen::Vector3f& getSweetSpot() const noexcept { return sweetSpot; }

#ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
#endif

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi()  const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int  getNumPrograms()                             override;
    int  getCurrentProgram()                          override;
    void setCurrentProgram(int)                      override;
    const juce::String getProgramName(int)           override;
    void changeProgramName(int, const juce::String&) override;

    void getStateInformation(juce::MemoryBlock& destData)       override;
    void setStateInformation(const void* data, int sizeInBytes) override;

private:
    VBAPProcessor vbap;
    Eigen::Matrix<float, 3, 3> altPosRows;
    Eigen::Vector3f sweetSpot { 0.0f, 0.0f, 0.0f };
    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float> lastGains[3]{ {0.33f}, {0.33f}, {0.33f} };

    // Punteros atomicos cacheados de APVTS para evitar lookups por hash dentro
    // de processBlock. Resueltos una sola vez en el constructor.
    std::atomic<float>* pSrcX    = nullptr;
    std::atomic<float>* pSrcY    = nullptr;
    std::atomic<float>* pSrcZ    = nullptr;
    std::atomic<float>* pDownmix = nullptr;
    std::atomic<float>* pSwapLR  = nullptr;
    std::atomic<float>* pDelayCEnabled = nullptr;

    // Buffer mono preasignado en prepareToPlay para no allocar en processBlock.
    juce::AudioBuffer<float> monoInBuf;

    // Suavizado de ganancias VBAP (evita clicks al mover el Src)
    // SmoothedValue hace una rampa lineal hacia el valor objetivo
    // en cada bloque de audio - 50 ms de tiempo de transicion
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothGain[3];

    // Suavizado del delay del Center (en muestras) para evitar zipper
    // noise al arrastrar el slider; rampa de 20 ms.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothDelayC;

    // Delay de alineacion por canal (en muestras) via buffer circular.
    // Tamaño cubre kMaxDelayMs a 96 kHz (margen para SR altos del host).
    static constexpr int kMaxDelaySamples =
        static_cast<int>(kMaxDelayMs * 0.001f * 96000.0f);
    std::array<std::array<float, kMaxDelaySamples>, kNumSpeakers> delayBuffer {};
    std::array<int, kNumSpeakers> delayWritePos {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VBAPAudioProcessor)
};