/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include <Eigen/Dense>
#include "VBAPProcessor.h"

//==============================================================================
class VBAPAudioProcessor  : public juce::AudioProcessor
{
public:
    VBAPAudioProcessor();
    ~VBAPAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    // Getter para usar apvts desde el Editor
    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts; }

    // Layout de parámetros (azimuth, elevation, gain)
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

private:
    // Motor de procesamiento
    VBAPProcessor vbap;
    Eigen::Matrix<float,3,3> altPosRows;

    // Aquí declaramos el apvts
    juce::AudioProcessorValueTreeState apvts { *this, nullptr, "Parameters", createParameterLayout() };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VBAPAudioProcessor)
};
