#include "PluginProcessor.h"
#include "PluginEditor.h"

using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

//==============================================================================

VBAPAudioProcessorEditor::VBAPAudioProcessorEditor(VBAPAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    setSize(700, 500);
    setResizable(false, false);

    // === Azimuth Slider ===
    azimuthSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    azimuthSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 20);
    azimuthSlider.setRange(-90.0, 90.0, 0.1);
    addAndMakeVisible(azimuthSlider);
    azimuthAttachment = std::make_unique<SliderAttachment>(audioProcessor.getAPVTS(), "azimuth", azimuthSlider);

    // === Elevation Slider ===
    elevationSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    elevationSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 20);
    elevationSlider.setRange(-90.0, 90.0, 0.1);
    addAndMakeVisible(elevationSlider);
    elevationAttachment = std::make_unique<SliderAttachment>(audioProcessor.getAPVTS(), "elevation", elevationSlider);

    // === Gain Slider ===
    gainSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    gainSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 20);
    gainSlider.setRange(0.0, 2.0, 0.01);
    addAndMakeVisible(gainSlider);
    gainAttachment = std::make_unique<SliderAttachment>(audioProcessor.getAPVTS(), "gain", gainSlider);

    // === Downmix Button ===
    downmixButton.setButtonText("Downmix Stereo");
    addAndMakeVisible(downmixButton);
    downmixAttachment = std::make_unique<ButtonAttachment>(audioProcessor.getAPVTS(), "downmix", downmixButton);

    startTimerHz(30); // para refrescar posición visual
}

VBAPAudioProcessorEditor::~VBAPAudioProcessorEditor() {}

//==============================================================================

void VBAPAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour::fromRGB(30, 38, 41));

    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(18.0f, juce::Font::bold));
    g.drawFittedText("VBAP Spatializer", getLocalBounds().removeFromTop(40), juce::Justification::centred, 1);

    // === Área del gráfico 3D ===
    vbapArea = getLocalBounds().reduced(60, 60).removeFromLeft(getWidth() / 2 + 40);
    g.setColour(juce::Colour::fromRGB(40, 48, 52));
    g.fillRoundedRectangle(vbapArea.toFloat(), 10.0f);
    g.setColour(juce::Colour::fromRGB(70, 80, 85));
    g.drawRoundedRectangle(vbapArea.toFloat(), 10.0f, 2.0f);

    // === Triángulo de altavoces ===
    // calculamos L,R,T usando coordenadas válidas de Rectangle
    auto L = vbapArea.getBottomLeft().toFloat().translated(50.0f, -50.0f);
    auto R = vbapArea.getBottomRight().toFloat().translated(-50.0f, -50.0f);

    // --- CORRECCIÓN: no usar getTop() --
    float tx = static_cast<float>(vbapArea.getX()) + vbapArea.getWidth() * 0.5f;
    float ty = static_cast<float>(vbapArea.getY()) + 60.0f;
    auto T = juce::Point<float>(tx, ty);

    g.setColour(juce::Colour::fromRGB(90, 100, 110));
    g.drawLine(L.x, L.y, R.x, R.y, 1.5f);
    g.drawLine(L.x, L.y, T.x, T.y, 1.5f);
    g.drawLine(R.x, R.y, T.x, T.y, 1.5f);

    g.setColour(juce::Colours::lightblue);
    g.fillEllipse(L.x - 6, L.y - 6, 12, 12);
    g.fillEllipse(R.x - 6, R.y - 6, 12, 12);
    g.fillEllipse(T.x - 6, T.y - 6, 12, 12);

    g.setColour(juce::Colours::white);
    g.drawFittedText("L", juce::Rectangle<int>(static_cast<int>(L.x) - 20, static_cast<int>(L.y) + 8, 40, 20), juce::Justification::centred, 1);
    g.drawFittedText("R", juce::Rectangle<int>(static_cast<int>(R.x) - 20, static_cast<int>(R.y) + 8, 40, 20), juce::Justification::centred, 1);
    g.drawFittedText("T", juce::Rectangle<int>(static_cast<int>(T.x) - 10, static_cast<int>(T.y) - 25, 20, 20), juce::Justification::centred, 1);

    // === Fuente (SRC) ===
    auto srcScreen = sourceToScreen(srcPosNorm);
    g.setColour(juce::Colours::orange);
    g.fillEllipse(srcScreen.x - 10, srcScreen.y - 10, 20, 20);
    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(14.0f));
    g.drawFittedText("SRC", juce::Rectangle<int>(static_cast<int>(srcScreen.x) - 20, static_cast<int>(srcScreen.y) + 12, 40, 20),
                     juce::Justification::centred, 1);
}

void VBAPAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds().reduced(20);
    auto rightPanel = bounds.removeFromRight(220);

    int knobHeight = 120;
    int spacing = 20;

    azimuthSlider.setBounds(rightPanel.removeFromTop(knobHeight).reduced(40, 10));
    elevationSlider.setBounds(rightPanel.removeFromTop(knobHeight).reduced(40, 10));
    gainSlider.setBounds(rightPanel.removeFromTop(knobHeight).reduced(40, 10));

    rightPanel.removeFromTop(spacing);
    downmixButton.setBounds(rightPanel.removeFromTop(40).reduced(40, 10));
}

//==============================================================================
// Conversión coordenadas
//==============================================================================

juce::Point<float> VBAPAudioProcessorEditor::sourceToScreen(const juce::Point<float>& src)
{
    float cx = vbapArea.getCentreX();
    float cy = vbapArea.getCentreY();
    float halfW = vbapArea.getWidth() * 0.35f;
    float halfH = vbapArea.getHeight() * 0.35f;

    return { cx + src.x * halfW, cy - src.y * halfH };
}

juce::Point<float> VBAPAudioProcessorEditor::screenToSource(const juce::Point<float>& screen)
{
    float cx = vbapArea.getCentreX();
    float cy = vbapArea.getCentreY();
    float halfW = vbapArea.getWidth() * 0.35f;
    float halfH = vbapArea.getHeight() * 0.35f;

    float xNorm = juce::jlimit(-1.0f, 1.0f, (screen.x - cx) / halfW);
    float yNorm = juce::jlimit(-1.0f, 1.0f, (cy - screen.y) / halfH);
    return { xNorm, yNorm };
}

// ==============================================================================
//  Interacción con el punto SRC (moverlo con el mouse y sincronizar sliders)
// ==============================================================================
// mouseDown: iniciar drag cuando haces click cerca del punto SRC o dentro del área
void VBAPAudioProcessorEditor::mouseDown(const juce::MouseEvent& e)
{
    if (! vbapArea.contains(e.getPosition()))
        return;

    auto posF = e.position.toFloat();
    auto srcScreen = sourceToScreen(srcPosNorm);

    // si hiciste click cerca del punto, arrancamos drag
    if (srcScreen.getDistanceFrom(posF) < 18.0f)
    {
        isDraggingSource = true;
    }
    else
    {
        // si clickeaste en el área, también movemos el SRC allí y empezamos drag
        isDraggingSource = true;
        srcPosNorm = screenToSource(posF);

        // actualizar sliders (esto actualiza el APVTS vía SliderAttachment)
        float az = juce::jlimit(-90.0f, 90.0f, srcPosNorm.x * 90.0f);
        float el = juce::jlimit(-90.0f, 90.0f, srcPosNorm.y * 90.0f);
        azimuthSlider.setValue(az, juce::NotificationType::sendNotification);
        elevationSlider.setValue(el, juce::NotificationType::sendNotification);
    }

    repaint();
}

// mouseDrag: arrastrar el punto y actualizar parámetros (sin tocar atomics directamente)
void VBAPAudioProcessorEditor::mouseDrag(const juce::MouseEvent& e)
{
    if (!isDraggingSource || !vbapArea.contains(e.getPosition()))
        return;

    auto posF = e.position.toFloat();
    srcPosNorm = screenToSource(posF);

    // mapear a grados
    float az = juce::jlimit(-90.0f, 90.0f, srcPosNorm.x * 90.0f);
    float el = juce::jlimit(-90.0f, 90.0f, srcPosNorm.y * 90.0f);

    // actualizamos sliders VISUALMENTE con sendNotification -> esto hace que el APVTS reciba el cambio
    azimuthSlider.setValue(az, juce::NotificationType::sendNotification);
    elevationSlider.setValue(el, juce::NotificationType::sendNotification);

    repaint();
}

void VBAPAudioProcessorEditor::mouseUp(const juce::MouseEvent& )
{
    isDraggingSource = false;
}

// ==============================================================================
//  Actualización visual automática (cuando cambian knobs o parámetros del host)
// ==============================================================================
void VBAPAudioProcessorEditor::timerCallback()
{
    // leer los parámetros desde APVTS sin copiar atomics
    float az = audioProcessor.getAPVTS().getRawParameterValue("azimuth")->load();
    float el = audioProcessor.getAPVTS().getRawParameterValue("elevation")->load();

    // mapear a normalizado [-1..1] que usa sourceToScreen/screenToSource
    srcPosNorm.x = juce::jlimit(-1.0f, 1.0f, az / 90.0f);
    srcPosNorm.y = juce::jlimit(-1.0f, 1.0f, el / 90.0f);

    repaint();
}
