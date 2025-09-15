class VBAPAudioProcessorEditor  : public juce::AudioProcessorEditor,
                                  private juce::Timer
{
public:
    VBAPAudioProcessorEditor (VBAPAudioProcessor&);
    ~VBAPAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void timerCallback() override;

    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

private:
    VBAPAudioProcessor& audioProcessor;

    juce::Slider azimuthSlider, elevationSlider, gainSlider;
    juce::ToggleButton downmixButton;

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> azimuthAttachment, elevationAttachment, gainAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> downmixAttachment;

    juce::Rectangle<int> vbapArea;

    juce::Point<float> srcPosNorm {0.0f, 0.0f};
    bool isDraggingSource = false;

    juce::Point<float> sourceToScreen(const juce::Point<float>& src);
    juce::Point<float> screenToSource(const juce::Point<float>& screen);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VBAPAudioProcessorEditor)
};
