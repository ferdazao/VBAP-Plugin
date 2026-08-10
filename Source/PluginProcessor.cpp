// PluginProcessor.cpp  -  VBAP Spatializer

#include "PluginProcessor.h"
#include "PluginEditor.h"

VBAPAudioProcessor::VBAPAudioProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::mono(), true)
        .withOutput("Output", juce::AudioChannelSet::createLCR(), true)),
    apvts(*this, nullptr, "Parameters", createParameterLayout())
{
    // Cachear punteros atomicos: createParameterLayout ya corrio,
    // los IDs estan garantizados. Cero lookups en processBlock.
    pSrcX    = apvts.getRawParameterValue("sourceX");
    pSrcY    = apvts.getRawParameterValue("sourceY");
    pSrcZ    = apvts.getRawParameterValue("sourceZ");
    pDownmix = apvts.getRawParameterValue("downmix");
    pSwapLR  = apvts.getRawParameterValue("swapLR");
    pDelayCEnabled = apvts.getRawParameterValue("delayCEnabled");
}

VBAPAudioProcessor::~VBAPAudioProcessor() {}

juce::AudioProcessorValueTreeState::ParameterLayout
VBAPAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"sourceX", 1}, "Source X",
        juce::NormalisableRange<float>(-kSpeakerXAbs, kSpeakerXAbs, 0.01f), 0.f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"sourceY", 1}, "Source Y", juce::NormalisableRange<float>(2.42f, 6.f, 0.01f), 4.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"sourceZ", 1}, "Source Z",
        juce::NormalisableRange<float>(kListenerZ, kSpeakerZMax, 0.01f), kListenerZ));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"downmix", 1}, "Downmix Stereo", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"swapLR", 1}, "Intercambiar L/R", false));

    // Toggle on/off del delay de alineacion del Center. Cuando esta activo se
    // aplica un delay fijo de kCenterDelayMs (0.73 ms) que compensa la
    // diferencia de distancia C vs L/R con la geometria por defecto. Antes
    // era un slider 0-50 ms; el slider se removio porque el valor optimo es
    // fijo dado el setup de altavoces.
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"delayCEnabled", 1}, "Delay Center 0.73 ms", false));

    return { params.begin(), params.end() };
}

const juce::String VBAPAudioProcessor::getName() const { return JucePlugin_Name; }
bool VBAPAudioProcessor::acceptsMidi()  const { return false; }
bool VBAPAudioProcessor::producesMidi() const { return false; }
bool VBAPAudioProcessor::isMidiEffect() const { return false; }
double VBAPAudioProcessor::getTailLengthSeconds() const { return 0.0; }
int  VBAPAudioProcessor::getNumPrograms() { return 1; }
int  VBAPAudioProcessor::getCurrentProgram() { return 0; }
void VBAPAudioProcessor::setCurrentProgram(int) {}
const juce::String VBAPAudioProcessor::getProgramName(int) { return {}; }
void VBAPAudioProcessor::changeProgramName(int, const juce::String&) {}

void VBAPAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    // Posiciones de altavoces (m). Filas = L, R, C.
    // Geometria: oyente en (0,0,kListenerZ=1.20). L y R a ~2.80m del oyente,
    // separacion entre ellos 2.80m. C a ~2.55m del oyente, centrado y elevado
    // a 2m de altura absoluta. Angulos desde el oyente:
    //   L: az=-30°, el=+2°   C: az=0°, el=+18°   R: az=+30°, el=+2°
    altPosRows << -1.40f, 2.42f, 1.30f,
                   1.40f, 2.42f, 1.30f,
                   0.00f, 2.42f, 2.00f;

    // Sweet spot = midpoint(L, R) en coordenadas de mundo. Origen del escalar
    // de distancia para la atenuacion y el slider "Distancia [m]". Se recalcula
    // aqui cada vez que se redefinen las posiciones de altavoz.
    sweetSpot = 0.5f * (altPosRows.row(0).transpose() + altPosRows.row(1).transpose());

    // VBAP requiere posiciones RELATIVAS al oyente para que la direccion
    // a la fuente (que tambien se trasla en processBlock) este en el mismo
    // marco de referencia. Sin este offset, las ganancias salen incoherentes
    // y C suele clampearse a 0. Nota: NO se traslada por sweetSpot porque
    // L, R y C son coplanares en Y=2.42 = sweetSpotY => det(A) = 0.
    Eigen::Matrix<float, 3, 3> altRel = altPosRows;
    altRel.col(2).array() -= kListenerZ;
    vbap.setSpeakerPositions(altRel);
    vbap.setSampleRate(sampleRate);

    // Preasignar el buffer mono para no allocar en processBlock.
    monoInBuf.setSize(1, samplesPerBlock, false, false, true);
    monoInBuf.clear();

    // Inicializar el suavizador de ganancias
    // 0.05 = 50 ms de rampa - suficiente para eliminar clicks
    // sin que el panning se sienta "lento"
    for (int i = 0; i < 3; ++i)
    {
        smoothGain[i].reset(sampleRate, 0.05);
        smoothGain[i].setCurrentAndTargetValue(0.0f);
    }

    // Suavizado del delay del Center: rampa de 20 ms (zipper-free).
    smoothDelayC.reset(sampleRate, 0.02);
    smoothDelayC.setCurrentAndTargetValue(0.0f);

    // Inicializar buffers de delay de alineacion
    for (auto& ring : delayBuffer) ring.fill(0.0f);
    delayWritePos.fill(0);
}

void VBAPAudioProcessor::releaseResources() {}

#ifndef JucePlugin_PreferredChannelConfigurations
bool VBAPAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // Entrada: mono o estereo. Cubase (y otros hosts VST3) suelen instanciar
    // los plugins en pistas estereo por defecto; rechazar estereo rompe la
    // carga directa en esas pistas. Si llega estereo, processBlock lo colapsa
    // a mono promediando L+R (ver bucle de mixdown).
    const auto& in = layouts.getMainInputChannelSet();
    if (in != juce::AudioChannelSet::mono()
        && in != juce::AudioChannelSet::stereo())
        return false;

    // Salida LCR (3.0): SpeakerArrangement VST3 estandar que Reaper, Cubase
    // y demas reconocen durante el scan. discreteChannels(3) queda como
    // fallback solo para sesiones guardadas previas a la migracion a LCR.
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::createLCR()
        || out == juce::AudioChannelSet::discreteChannels(3);
}
#endif

void VBAPAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    const int numInChs = getTotalNumInputChannels();
    const int numOutChs = getTotalNumOutputChannels();
    if (numSamples == 0 || numOutChs < 3) return;

    const float srcX   = pSrcX->load();
    const float srcY   = pSrcY->load();
    const float srcZ   = pSrcZ->load();
    const bool  dmix   = pDownmix->load() > 0.5f;
    const bool  swapLR = pSwapLR->load() > 0.5f;

    // Atenuacion 1/r reescalada referenciada al sweet spot. Endpoints garantizados:
    //   D <= kRefDistance (1 m) -> 0 dB; D >= kMaxDistance -> kMinGainDb (-24 dB).
    // El denominador log10(kMaxDistance/kRefDistance) = log10(8) ≈ 0.903.
    const Eigen::Vector3f srcWorld(srcX, srcY, srcZ);
    const float D        = (srcWorld - sweetSpot).norm();
    const float dClamped = juce::jmax(D, kRefDistance);
    const float gainDb   = juce::jlimit(kMinGainDb, 0.0f,
        kMinGainDb * std::log10(dClamped    / kRefDistance)
                   / std::log10(kMaxDistance / kRefDistance));
    const float gain     = juce::Decibels::decibelsToGain(gainDb);

    // VBAP opera con direcciones desde el oyente. Trasladamos la fuente
    // restando la altura del oyente: (X, Y, Z) -> (X, Y, Z - kListenerZ).
    Eigen::Vector3f srcPos(srcX, srcY, srcZ - kListenerZ);

    // Calcular ganancias VBAP target (con clamp de negativos ya aplicado en VBAPProcessor)
    auto g = vbap.computeGainsXYZ(srcPos);
    // Publicamos la ganancia efectiva por canal (VBAP * distancia) para que las
    // barras del editor reflejen lo que realmente suena. Sin el factor de
    // distancia, mover sourceY no movia las barras (las VBAP estan energy-norm).
    for (int ch = 0; ch < 3; ++ch) lastGains[ch].store(g(ch) * gain);

    // Fijar valor objetivo de los suavizadores.
    // Cada SmoothedValue hara una rampa de 50 ms hacia este target.
    for (int ch = 0; ch < 3; ++ch)
        smoothGain[ch].setTargetValue(g(ch) * gain);

    // Construir buffer mono a partir del input del bus (promedio de canales).
    // monoInBuf preasignado en prepareToPlay -> cero allocs aqui.
    // Escalar por 1/numInChs en lugar de sumar crudo: si llega estereo
    // (Cubase suele instanciar en pistas estereo) L+R/2 mantiene el mismo
    // nivel percibido que mono. Mono pasa intacto (1/1 = 1).
    jassert(numSamples <= monoInBuf.getNumSamples());
    float* monoIn = monoInBuf.getWritePointer(0);
    juce::FloatVectorOperations::clear(monoIn, numSamples);
    const float invInChs = (numInChs > 0)
        ? 1.0f / static_cast<float>(numInChs) : 1.0f;
    for (int ch = 0; ch < numInChs; ++ch)
        juce::FloatVectorOperations::addWithMultiply(
            monoIn, buffer.getReadPointer(ch), invInChs, numSamples);

    // SwapLR resuelto mediante mapeo logico->fisico: ganancia logica i va al
    // output fisico outOrder[i]. Elimina el bucle muestra-a-muestra de swap.
    const int outOrder[3] = { swapLR ? 1 : 0, swapLR ? 0 : 1, 2 };

    // Aplicar ganancias suavizadas con la API SIMD-friendly de SmoothedValue.
    // copy() sobrescribe el buffer (no hace falta clear() previo); applyGain()
    // rampa internamente sin getNextValue() por muestra.
    for (int i = 0; i < 3; ++i)
    {
        float* out = buffer.getWritePointer(outOrder[i]);
        juce::FloatVectorOperations::copy(out, monoIn, numSamples);
        smoothGain[i].applyGain(out, numSamples);
    }

    float* outL = buffer.getWritePointer(0);
    float* outR = buffer.getWritePointer(1);
    float* outC = buffer.getWritePointer(2);

    // Delay de alineacion del Center via buffer circular vectorizado.
    // L y R no tienen delay (siempre 0); solo el Center se procesa.
    // Toggle on/off: cuando activo se aplica kCenterDelayMs fijo, sino 0.
    // El SmoothedValue mantiene la rampa de 20 ms para evitar transientes
    // al togglear (sin esto, encender el toggle insertaria de golpe ~35
    // muestras de offset a 48k y se oiria un click).
    const float targetDelaySamples = (pDelayCEnabled->load() > 0.5f)
        ? kCenterDelayMs * 0.001f * static_cast<float>(getSampleRate())
        : 0.0f;
    smoothDelayC.setTargetValue(targetDelaySamples);
    smoothDelayC.skip(numSamples);
    const int dly = juce::jmin(static_cast<int>(smoothDelayC.getCurrentValue()),
                               kMaxDelaySamples - 1);

    if (dly > 0)
    {
        auto& ring = delayBuffer[2];
        constexpr int N = kMaxDelaySamples;
        const int wp = delayWritePos[2];

        // Escribir el bloque al ring (con posible wrap en dos chunks).
        const int firstWrite = std::min(numSamples, N - wp);
        juce::FloatVectorOperations::copy(ring.data() + wp, outC, firstWrite);
        if (firstWrite < numSamples)
            juce::FloatVectorOperations::copy(ring.data(), outC + firstWrite,
                                              numSamples - firstWrite);

        // Leer el bloque retardado al output (con posible wrap).
        const int rp = (wp - dly + N) % N;
        const int firstRead = std::min(numSamples, N - rp);
        juce::FloatVectorOperations::copy(outC, ring.data() + rp, firstRead);
        if (firstRead < numSamples)
            juce::FloatVectorOperations::copy(outC + firstRead, ring.data(),
                                              numSamples - firstRead);

        delayWritePos[2] = (wp + numSamples) % N;
    }

    if (dmix) {
        juce::FloatVectorOperations::addWithMultiply(outL, outC, 0.5f, numSamples);
        juce::FloatVectorOperations::addWithMultiply(outR, outC, 0.5f, numSamples);
        juce::FloatVectorOperations::clear(outC, numSamples);
    }
}

bool VBAPAudioProcessor::hasEditor() const { return true; }

juce::AudioProcessorEditor* VBAPAudioProcessor::createEditor()
{
    return new VBAPAudioProcessorEditor(*this);
}

void VBAPAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void VBAPAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml && xml->hasTagName(apvts.state.getType()))
        apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VBAPAudioProcessor();
}