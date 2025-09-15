/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <sstream>


//==============================================================================
VBAPAudioProcessor::VBAPAudioProcessor()
    : AudioProcessor (BusesProperties()
                      .withInput ("Input",  juce::AudioChannelSet::mono(), true)
                      .withOutput("Output", juce::AudioChannelSet::discreteChannels(3), true)
                      ),
      apvts(*this, nullptr, "Parameters", createParameterLayout())
{
}

VBAPAudioProcessor::~VBAPAudioProcessor()
{
}

juce::AudioProcessorValueTreeState::ParameterLayout VBAPAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back (std::make_unique<juce::AudioParameterFloat>(
        "azimuth", "Azimuth",
        juce::NormalisableRange<float>(-90.0f, 90.0f, 1.0f),
        0.0f
    ));

    params.push_back (std::make_unique<juce::AudioParameterFloat>(
        "elevation", "Elevation",
        juce::NormalisableRange<float>(-90.0f, 90.0f, 0.1f),
        0.0f
    ));

    params.push_back (std::make_unique<juce::AudioParameterFloat>(
        "gain", "Gain",
        juce::NormalisableRange<float>(0.0f, 2.0f, 0.01f),
        1.0f
    ));
    
    // Crear un downmix a estereo
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        "downmix",   // ID
        "Downmix",   // nombre visible en host
        false        // valor por defecto = OFF
    ));

    return { params.begin(), params.end() };
}

//==============================================================================
const juce::String VBAPAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool VBAPAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool VBAPAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool VBAPAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double VBAPAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int VBAPAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int VBAPAudioProcessor::getCurrentProgram()
{
    return 0;
}

void VBAPAudioProcessor::setCurrentProgram (int index)
{
}

const juce::String VBAPAudioProcessor::getProgramName (int index)
{
    return {};
}

void VBAPAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
}

//==============================================================================
void VBAPAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // 1. Configurar primero las posiciones de los altavoces
    altPosRows = Eigen::MatrixXf(3,3);
    altPosRows <<  -0.85f, 1.0f, 1.2f,   // Left
                    0.0f, 1.0f, 2.78f,   // Top
                    0.85f, 1.0f, 1.2f;   // Right

    // 2. Pasar la matriz a VBAP (ya inicializada)
    vbap.setSpeakerPositions(altPosRows);

    // 3. Configurar sample rate y bloque
    vbap.setSampleRate(sampleRate, samplesPerBlock, /*maxDistMeters=*/10.0f, /*c=*/343.0f);
}

void VBAPAudioProcessor::releaseResources()
{
    // When playback stops, you can use this as an opportunity to free up any
    // spare memory, etc.
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool VBAPAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // aceptamos solo: 1 canal de entrada (mono) y 3 canales de salida
    if (layouts.getMainInputChannelSet()  != juce::AudioChannelSet::mono()
     || layouts.getMainOutputChannelSet() != juce::AudioChannelSet::discreteChannels(3))
        return false;

    return true;
}
#endif

void VBAPAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    if (numSamples == 0) return;  // evitar accesos si el bloque llega vacío

    const int numInputChannels  = getTotalNumInputChannels();
    const int numOutputChannels = getTotalNumOutputChannels();
    jassert(numOutputChannels >= 3);

    auto* azParam = apvts.getRawParameterValue("azimuth");
    auto* elParam = apvts.getRawParameterValue("elevation");
    auto* gParam  = apvts.getRawParameterValue("gain");

    jassert (azParam != nullptr);
    jassert (elParam != nullptr);
    jassert (gParam  != nullptr);

    float azimuth   = azParam->load();
    float elevation = elParam->load();
    float gain      = gParam->load();
    
    {
        std::ostringstream oss;
        oss << "param ptrs: az=" << static_cast<const void*>(azParam)
            << " el=" << static_cast<const void*>(elParam)
            << " g=" << static_cast<const void*>(gParam);
        DBG(oss.str());

        DBG("param values: az=" << azimuth
            << " el=" << elevation
            << " g=" << gain);
    }

    // calcular posición de la fuente (1m de radio en esfera, coherente con directionVectorDeg)
    float radiusMeters = 1.0f;
    float az = juce::degreesToRadians(azimuth);
    float el = juce::degreesToRadians(elevation);

    Eigen::Vector3f srcPosMeters;
    // Convención: az=0 frente (+Y), +90 izquierda (-X), -90 derecha (+X)
    srcPosMeters(0) = -radiusMeters * std::cos(el) * std::sin(az); // X
    srcPosMeters(1) =  radiusMeters * std::cos(el) * std::cos(az); // Y
    srcPosMeters(2) =  radiusMeters * std::sin(el);                // Z

    // apuntadores a las 3 salidas
    float* out0 = buffer.getWritePointer(0);
    float* out1 = buffer.getWritePointer(1);
    float* out2 = buffer.getWritePointer(2);

    // buffer mono de entrada sumado
    std::vector<float> summed(numSamples, 0.0f);
    for (int ch=0; ch<numInputChannels; ++ch)
    {
        const float* inPtr = buffer.getReadPointer(ch);
        for (int n=0; n<numSamples; ++n)
            summed[n] += inPtr[n];
    }

    // limpiar salidas
    for (int ch=0; ch<3; ++ch)
        buffer.clear(ch, 0, numSamples);
    
    // --- DEBUG: imprimir valores cada N bloques para no spamear la consola ---
    static int debugBlockCounter = 0;
    const int debugEveryNBlocks = 40; // ajustar: cada 40 bloques imprime una línea

    if (++debugBlockCounter >= debugEveryNBlocks)
    {
        debugBlockCounter = 0;

        // obtener gains calculadas (no hace work pesado)
        Eigen::Vector3f dbgGains = vbap.computeGainsDeg(azimuth, elevation);

        // construir string con ostringstream (Eigen es streamable a std::ostream)
        std::ostringstream oss;
        oss << "az=" << azimuth << " el=" << elevation << " gain=" << gain << " | ";
        oss << "srcPos: [" << srcPosMeters(0) << " " << srcPosMeters(1) << " " << srcPosMeters(2) << "] | ";
        oss << "gains: [" << dbgGains.transpose() << "] | ";
        oss << "speakers:\n" << vbap.getSpeakerMatrix();

        DBG(oss.str()); // juce::DBG acepta std::string convertible
    }

    // procesar en VBAP
    vbap.processBlockFractionalDelay(
        summed.data(), numSamples,
        out0, out1, out2,
        altPosRows, srcPosMeters,
        azimuth, elevation, gain
    );
    
    // Leer el parámetro downmix
    bool downmix = apvts.getRawParameterValue("downmix")->load() > 0.5f;

    if (downmix && numOutputChannels >= 2)
    {
        // Creamos buffers temporales para leer las 3 salidas
        auto* out0 = buffer.getWritePointer(0);
        auto* out1 = buffer.getWritePointer(1);
        auto* out2 = buffer.getWritePointer(2);

        const float topGain = 0.70710678f;

        for (int n = 0; n < numSamples; ++n)
        {
            float left  = out0[n] + topGain * out1[n];   // L + Top
            float right = out2[n] + topGain * out1[n];  // R + Top

            out0[n] = left;
            out1[n] = right;
        }

        // Limpia el canal 3 (no se usa en estéreo)
        buffer.clear(2, 0, numSamples);
    }

    // limpiar cualquier salida extra (si existiera)
    for (int ch=3; ch<numOutputChannels; ++ch)
        buffer.clear(ch, 0, numSamples);
}

//==============================================================================
bool VBAPAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* VBAPAudioProcessor::createEditor()
{
    return new VBAPAudioProcessorEditor (*this);
}

//==============================================================================
void VBAPAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // You should use this method to store your parameters in the memory block.
    // You could do that either as raw data, or use the XML or ValueTree classes
    // as intermediaries to make it easy to save and load complex data.
}

void VBAPAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // You should use this method to restore your parameters from this memory block,
    // whose contents will have been created by the getStateInformation() call.
}

//==============================================================================
// This creates new instances of the plugin..

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VBAPAudioProcessor();
}

