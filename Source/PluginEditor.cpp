// PluginEditor.cpp  -  VBAP Spatializer  -  CAMARA ORBITAL 3D
// ============================================================
// NUEVO: camara orbital con matriz de rotacion real
//
//   MOUSE sobre la vista 3D:
//     - Arrastrar en espacio vacio  -> rota camara (azimut + elevacion)
//     - Arrastrar sobre Src         -> mueve fuente sonora
//     - Doble-click                 -> reset camara a vista por defecto
//     - Boton "Reset Vista"         -> idem
//
//   Proyeccion: ortografica con matriz de rotacion de camara
//     rightVec = (cos(az),  -sin(az),  0)
//     upVec    = (sin(az)*sin(el), cos(az)*sin(el), cos(el))
//     screenX  = center.x + dot(worldOffset, rightVec) * scale
//     screenY  = center.y - dot(worldOffset, upVec)   * scale
//
//   Inversa analitica (para el drag del Src):
//     Cramer con det = sin(el), valido mientras el > 0
// ============================================================

#include "PluginProcessor.h"
#include "PluginEditor.h"

using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;

// ======== COLORES ============================================================
static const juce::Colour C_BG{ 10u,  14u,  22u };
static const juce::Colour C_FLOOR_F{ 25u,  38u,  58u };
static const juce::Colour C_FLOOR_B{ 18u,  28u,  45u };
static const juce::Colour C_WALL_BOT{ 15u,  23u,  38u };
static const juce::Colour C_WALL_TOP{ 26u,  38u,  58u };
static const juce::Colour C_GRID{ 45u,  70u, 110u };
static const juce::Colour C_EDGE{ 60u,  90u, 130u };
static const juce::Colour C_PANEL{ 22u,  32u,  48u };
static const juce::Colour C_SPK{ 70u, 165u, 245u };
static const juce::Colour C_SRC{ 255u, 140u,  20u };
static const juce::Colour C_TEXT = juce::Colours::white;
static const juce::Colour C_GAIN{ 60u, 200u, 110u };
static const juce::Colour C_NEG = juce::Colours::orangered;
static const juce::Colour C_AZ{ 100u, 220u, 180u };
static const juce::Colour C_RING{ 50u,  80u, 120u };
static const juce::Colour C_CAM_HINT{ 80u, 130u, 200u };  // pista de rotacion

// ======== LAYOUT =============================================================
static constexpr int RIGHT_W = 285;
static constexpr int TITLE_H = 32;
static constexpr int PAD = 10;
static constexpr int LABEL_H = 15;
static constexpr int SLIDER_H = 30;
static constexpr int ROW_H = LABEL_H + SLIDER_H + 5;

// Punto en el espacio mundo alrededor del cual orbita la camara.
// LOOKAT_Y = punto medio del rango Y valido para la fuente (2.5..6.0) =>
// el drag XY en pantalla se mapea simetricamente al rango util.
static constexpr float LOOKAT_X = 0.0f;
static constexpr float LOOKAT_Y = 4.25f;   // punto medio del nuevo rango Y valido (2.5..6.0)
static constexpr float LOOKAT_Z = 1.5f;

// Escala ortografica de la camara por defecto (ajustada para ROOM_Y = 6 m).
static constexpr float CAM_SCALE_DEFAULT = 70.0f;

//==============================================================================
//  Constructor
//==============================================================================
VBAPAudioProcessorEditor::VBAPAudioProcessorEditor(VBAPAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    setSize(kEditorW, kEditorH);
    setResizable(false, false);

    auto& apvts = audioProcessor.getAPVTS();

    // Cachear punteros atomicos APVTS para evitar hash lookups en paths
    // de mouse, timer y pintado (mismo patron que el processor).
    pSrcX   = apvts.getRawParameterValue("sourceX");
    pSrcY   = apvts.getRawParameterValue("sourceY");
    pSrcZ   = apvts.getRawParameterValue("sourceZ");
    pSwapLR = apvts.getRawParameterValue("swapLR");

    auto mkSlider = [&](juce::Slider& s, bool sph)
        {
            s.setSliderStyle(juce::Slider::LinearHorizontal);
            s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
            if (sph) {
                s.setColour(juce::Slider::thumbColourId, C_AZ);
                s.setColour(juce::Slider::trackColourId, C_AZ.withAlpha(0.32f));
                s.setColour(juce::Slider::backgroundColourId, C_GRID);
            }
            addAndMakeVisible(s);
        };

    mkSlider(azSlider, true);  azSlider.setRange(-AZ_MAX, AZ_MAX, 0.1);
    azSlider.setTextValueSuffix(" deg");
    azSlider.onValueChange = [this] { onSphericalSliderChanged(); };

    mkSlider(elSlider, true);  elSlider.setRange(0.0, 90.0, 0.1);
    elSlider.setTextValueSuffix(" deg");
    elSlider.onValueChange = [this] { onSphericalSliderChanged(); };

    mkSlider(distSlider, true);  distSlider.setRange(DIST_MIN, DIST_MAX, 0.05);
    distSlider.setTextValueSuffix(" m");
    distSlider.onValueChange = [this] { onSphericalSliderChanged(); };

    downmixButton.setButtonText("Downmix Stereo");
    addAndMakeVisible(downmixButton);
    downmixAtt = std::make_unique<ButtonAtt>(apvts, "downmix", downmixButton);

    swapLRButton.setButtonText("Intercambiar L/R");
    swapLRButton.setColour(juce::ToggleButton::textColourId, juce::Colour(255u, 200u, 100u));
    addAndMakeVisible(swapLRButton);
    swapLRAtt = std::make_unique<ButtonAtt>(apvts, "swapLR", swapLRButton);

    // Boton reset camara
    resetCamButton.setButtonText("Reset Vista");
    resetCamButton.setColour(juce::TextButton::buttonColourId, C_CAM_HINT.withAlpha(0.25f));
    resetCamButton.setColour(juce::TextButton::textColourOffId, C_CAM_HINT);
    resetCamButton.onClick = [this]
        {
            camAzimuth = CAM_AZ_DEFAULT;
            camElevation = CAM_EL_DEFAULT;
            camScale = CAM_SCALE_DEFAULT;
            updateCameraTrig();
            // Los defaults coinciden con la preset Iso; marcarla activa para
            // que el highlight refleje el estado real de la camara.
            activePreset = CamPreset::Iso;
            updatePresetButtonStates();
            repaint();
        };
    addAndMakeVisible(resetCamButton);

    // Presets de camara (Iso / Top / Front / Side). buttonOnColourId y
    // textColourOnId definen el estado highlighted: setToggleState(true) los
    // activa sin necesidad de clickingTogglesState (mantenemos el click
    // siempre aplicando el preset, no togglendolo).
    auto mkPreset = [&](juce::TextButton& b, const char* text, CamPreset p)
        {
            b.setButtonText(text);
            b.setColour(juce::TextButton::buttonColourId,    C_CAM_HINT.withAlpha(0.18f));
            b.setColour(juce::TextButton::textColourOffId,   C_CAM_HINT.brighter(0.15f));
            b.setColour(juce::TextButton::buttonOnColourId,  C_CAM_HINT.withAlpha(0.85f));
            b.setColour(juce::TextButton::textColourOnId,    juce::Colours::white);
            b.onClick = [this, p] { applyPreset(p); };
            addAndMakeVisible(b);
        };
    mkPreset(presetIsoBtn,   "Iso",   CamPreset::Iso);
    mkPreset(presetTopBtn,   "Top",   CamPreset::Top);
    mkPreset(presetFrontBtn, "Front", CamPreset::Front);
    mkPreset(presetSideBtn,  "Side",  CamPreset::Side);
    updatePresetButtonStates();   // arranca con Iso activo (default de camara)

    // Toggle on/off del delay de alineacion del Center (valor fijo 0.73 ms,
    // definido en VBAPAudioProcessor::kCenterDelayMs). Antes era un slider
    // 0-50 ms; se simplifico a toggle porque el valor optimo depende solo
    // de la geometria fija de altavoces y no requiere ajuste por sesion.
    delayCButton.setButtonText("Delay Center 0.73 ms");
    delayCButton.setColour(juce::ToggleButton::textColourId,
                           juce::Colour(100u, 200u, 220u));
    addAndMakeVisible(delayCButton);
    delayCEnabledAtt = std::make_unique<ButtonAtt>(apvts, "delayCEnabled", delayCButton);

    // Inicializar faders desde APVTS
    float x0 = pSrcX->load();
    float y0 = pSrcY->load();
    float z0 = pSrcZ->load();
    float a0, e0, d0;
    xyzToSpherical(x0, y0, z0, a0, e0, d0);
    azSlider.setValue(a0, juce::dontSendNotification);
    elSlider.setValue(e0, juce::dontSendNotification);
    distSlider.setValue(d0, juce::dontSendNotification);

    updateCameraTrig();
    setWantsKeyboardFocus(true);
    startTimerHz(30);
}

void VBAPAudioProcessorEditor::updateCameraTrig() noexcept
{
    const float pi = juce::MathConstants<float>::pi;
    const float ar = camAzimuth   * pi / 180.0f;
    const float er = camElevation * pi / 180.0f;
    camCosAz = std::cos(ar);
    camSinAz = std::sin(ar);
    camCosEl = std::cos(er);
    camSinEl = std::sin(er);
}

void VBAPAudioProcessorEditor::applyPreset(CamPreset p)
{
    switch (p)
    {
        case CamPreset::Iso:   camAzimuth = 45.0f; camElevation = 30.0f;  break;
        case CamPreset::Top:   camAzimuth =  0.0f; camElevation = 89.5f;  break;
        case CamPreset::Front: camAzimuth =  0.0f; camElevation =  5.0f;  break;
        case CamPreset::Side:  camAzimuth = 90.0f; camElevation =  5.0f;  break;
    }
    camScale = CAM_SCALE_DEFAULT;
    updateCameraTrig();
    activePreset = p;
    updatePresetButtonStates();
    repaint();
}

void VBAPAudioProcessorEditor::updatePresetButtonStates() noexcept
{
    // Marca toggleState=true en el boton que coincide con activePreset; los
    // demas quedan en false. setToggleState con dontSendNotification evita
    // re-disparar onClick. Cuando activePreset == nullopt (usuario rotando
    // manualmente) ningun boton queda highlighted.
    const auto match = [this](CamPreset p)
    {
        return activePreset.has_value() && *activePreset == p;
    };
    presetIsoBtn  .setToggleState(match(CamPreset::Iso),   juce::dontSendNotification);
    presetTopBtn  .setToggleState(match(CamPreset::Top),   juce::dontSendNotification);
    presetFrontBtn.setToggleState(match(CamPreset::Front), juce::dontSendNotification);
    presetSideBtn .setToggleState(match(CamPreset::Side),  juce::dontSendNotification);
}

VBAPAudioProcessorEditor::~VBAPAudioProcessorEditor() {}

//==============================================================================
//  PROYECCION ORBITAL (ortografica con rotacion de camara)
//
//  El "look-at" point (LOOKAT_X, LOOKAT_Y, LOOKAT_Z) esta en el centro de
//  la pantalla (isoScreenCenter). Todos los puntos se proyectan como offset
//  respecto a ese punto.
//
//  Vectors de camara:
//    right = (cos(az), -sin(az),  0)
//    up    = (sin(az)*sin(el), cos(az)*sin(el), cos(el))
//
//  screen_x = isoScreenCenter.x + dot(offset, right) * camScale
//  screen_y = isoScreenCenter.y - dot(offset, up)    * camScale
//==============================================================================
juce::Point<float> VBAPAudioProcessorEditor::isoProj(float wx, float wy, float wz) const
{
    // Usa cache: camCosAz/camSinAz/camCosEl/camSinEl (sin sin/cos por punto).
    const float dx = wx - LOOKAT_X;
    const float dy = wy - LOOKAT_Y;
    const float dz = wz - LOOKAT_Z;

    const float ux = camSinAz * camSinEl;
    const float uy = camCosAz * camSinEl;

    const float sx = (dx * camCosAz - dy * camSinAz) * camScale;
    const float sy = -(dx * ux + dy * uy + dz * camCosEl) * camScale;

    return { isoScreenCenter.x + sx, isoScreenCenter.y + sy };
}

//==============================================================================
//  INVERSA ANALITICA (para el drag del Src)
//
//  Dado un punto de pantalla sp y wz conocido, calcula wx e wy:
//
//  Sistema 2x2 (Cramer):
//    cos(az)*dx - sin(az)*dy              = p1    [eje horizontal]
//    sin(az)*sin(el)*dx + cos(az)*sin(el)*dy = p2    [eje vertical, wz sustraido]
//
//  det = sin(el)  (singular solo cuando el = 0, nunca en la practica)
//==============================================================================
std::pair<float, float> VBAPAudioProcessorEditor::isoInverseXY(juce::Point<float> sp, float wz) const
{
    // Usa cache de trig de camara.
    const float dz = wz - LOOKAT_Z;

    const float p1 = (sp.x - isoScreenCenter.x) / camScale;
    const float p2 = -(sp.y - isoScreenCenter.y) / camScale - dz * camCosEl;

    const float a = camCosAz, b = -camSinAz;
    const float c = camSinAz * camSinEl, d = camCosAz * camSinEl;

    const float det = a * d - b * c;   // = sin(el)
    if (std::abs(det) < 1e-6f) return { 0.0f, LOOKAT_Y };

    const float ddx = (p1 * d - p2 * b) / det;
    const float ddy = (p2 * a - p1 * c) / det;

    const float wx = juce::jlimit(-SOURCE_X_MAX, SOURCE_X_MAX, ddx + LOOKAT_X);
    const float wy = juce::jlimit(DIST_MIN, DIST_MAX, ddy + LOOKAT_Y);
    return { wx, wy };
}

//==============================================================================
//  Conversiones parametro <-> XYZ
//
//  IMPORTANTE: los nombres "spherical" y "Spherical" son legados.
//  El mapeo NO es esferico: es lineal y totalmente desacoplado.
//
//     az ∈ [-AZ_MAX, AZ_MAX]       -> x = az * (SOURCE_X_MAX / AZ_MAX)
//     el ∈ [0, EL_MAX]             -> z = listenerZ + el * (SOURCE_Z_MAX - listenerZ) / EL_MAX
//                                        (el=0  -> nivel del oido (kListenerZ);
//                                         el=90 -> altura del Center (SOURCE_Z_MAX = 2.0 m))
//     dist ∈ [DIST_MIN, DIST_MAX]  -> y = dist  (profundidad lineal)
//
//  Garantia: cada slider afecta exactamente una coordenada cartesiana.
//  No hay sin/cos ni acoplamiento entre az, el y dist.
//
//  La distancia euclidea real al sweet spot (||source - sweetSpot||) NO se
//  refleja en este slider — se muestra en el readout inferior del area 3D.
//==============================================================================
void VBAPAudioProcessorEditor::sphericalToXYZ(float az, float el, float dist,
    float& x, float& y, float& z) const
{
    const float lz   = VBAPAudioProcessor::kListenerZ;
    // zRng usa SOURCE_Z_MAX (= altura del Center), no ROOM_Z: la fuente se
    // limita al rango de altura del triangulo de altavoces, igual que x se
    // limita a SOURCE_X_MAX (= posicion lateral de L/R).
    const float zRng = SOURCE_Z_MAX - lz;
    x = juce::jlimit(-SOURCE_X_MAX, SOURCE_X_MAX, az * (SOURCE_X_MAX / AZ_MAX));
    z = juce::jlimit(lz,            SOURCE_Z_MAX, lz + el * (zRng / EL_MAX));
    y = juce::jlimit(DIST_MIN,      DIST_MAX,     dist);
}

void VBAPAudioProcessorEditor::xyzToSpherical(float x, float y, float z,
    float& az, float& el, float& dist) const
{
    const float lz   = VBAPAudioProcessor::kListenerZ;
    const float zRng = SOURCE_Z_MAX - lz;
    az   = juce::jlimit(-AZ_MAX,   AZ_MAX,   x * (AZ_MAX / SOURCE_X_MAX));
    el   = juce::jlimit(0.0f,      EL_MAX,   (z - lz) * (EL_MAX / zRng));
    dist = juce::jlimit(DIST_MIN, DIST_MAX, y);
}

void VBAPAudioProcessorEditor::setXYZParameters(float x, float y, float z)
{
    auto& apvts = audioProcessor.getAPVTS();
    auto set = [&](const char* id, float v) {
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*>(apvts.getParameter(id)))
            p->setValueNotifyingHost(p->convertTo0to1(v));
        };
    set("sourceX", x); set("sourceY", y); set("sourceZ", z);
}

void VBAPAudioProcessorEditor::onSphericalSliderChanged()
{
    if (updatingFromTimer) return;
    float x, y, z;
    sphericalToXYZ((float)azSlider.getValue(), (float)elSlider.getValue(),
        (float)distSlider.getValue(), x, y, z);
    setXYZParameters(x, y, z);
    repaint();
}

//==============================================================================
//  resized()
//==============================================================================
void VBAPAudioProcessorEditor::resized()
{
    // LAYOUT:
    //  +---------------------------+----------+
    //  |   Vista 3D (isoArea)      |  Panel   |
    //  |                           | derecho  |
    //  +---------------------------+----------+

    const int W = getWidth();
    const int H = getHeight();

    // Vista 3D ocupa todo el alto disponible
    isoArea = { PAD, TITLE_H,
                W - RIGHT_W - PAD * 3,
                H - TITLE_H - PAD };

    isoScreenCenter = { (float)isoArea.getCentreX(),
                        (float)(isoArea.getCentreY() - 20) };

    // Panel derecho
    const int rx = W - RIGHT_W - PAD;
    const int sw = RIGHT_W - PAD * 2;
    const int sy0 = TITLE_H + PAD;

    azSlider.setBounds(rx + PAD, sy0 + LABEL_H + 2, sw, SLIDER_H);
    elSlider.setBounds(rx + PAD, sy0 + ROW_H + LABEL_H + 2, sw, SLIDER_H);
    distSlider.setBounds(rx + PAD, sy0 + ROW_H * 2 + LABEL_H + 2, sw, SLIDER_H);

    int btnY = sy0 + ROW_H * 3 + 6;
    downmixButton.setBounds(rx + PAD, btnY, sw, 26);
    swapLRButton.setBounds(rx + PAD, btnY + 30, sw, 26);
    resetCamButton.setBounds(rx + PAD, btnY + 60, sw, 26);

    // Fila de presets de camara (Iso / Top / Front / Side)
    const int btnRowY = btnY + 90;
    const int bw = (sw - 3 * 4) / 4;
    presetIsoBtn  .setBounds(rx + PAD + 0 * (bw + 4), btnRowY, bw, 22);
    presetTopBtn  .setBounds(rx + PAD + 1 * (bw + 4), btnRowY, bw, 22);
    presetFrontBtn.setBounds(rx + PAD + 2 * (bw + 4), btnRowY, bw, 22);
    presetSideBtn .setBounds(rx + PAD + 3 * (bw + 4), btnRowY, bw, 22);

    // Toggle delay Center (valor fijo 0.73 ms). Antes era un slider con
    // etiqueta encima; ahora es un solo ToggleButton (la etiqueta esta en
    // el texto del propio boton, consistente con downmix/swapLR).
    delayCButton.setBounds(rx + PAD, btnY + 122, sw, 26);

    // Ganancias VBAP en el panel derecho
    gainArea = { rx, btnY + 152, RIGHT_W, H - PAD - btnY - 152 };
}

//==============================================================================
//  fillQuad
//==============================================================================
void VBAPAudioProcessorEditor::fillQuad(juce::Graphics& g,
    juce::Point<float> a, juce::Point<float> b,
    juce::Point<float> c, juce::Point<float> d, juce::Colour col)
{
    juce::Path p;
    p.startNewSubPath(a); p.lineTo(b); p.lineTo(c); p.lineTo(d);
    p.closeSubPath();
    g.setColour(col);
    g.fillPath(p);
}

//==============================================================================
//  drawFloor
//==============================================================================
void VBAPAudioProcessorEditor::drawFloor(juce::Graphics& g)
{
    auto FL = isoProj(-ROOM_X, 0.0f, 0.0f);
    auto FR = isoProj(ROOM_X, 0.0f, 0.0f);
    auto BR = isoProj(ROOM_X, ROOM_Y, 0.0f);
    auto BL = isoProj(-ROOM_X, ROOM_Y, 0.0f);

    juce::Path floor;
    floor.startNewSubPath(FL); floor.lineTo(FR);
    floor.lineTo(BR); floor.lineTo(BL); floor.closeSubPath();

    juce::ColourGradient fg(C_FLOOR_F, FL.x, FL.y, C_FLOOR_B, BL.x, BL.y, false);
    g.setGradientFill(fg);
    g.fillPath(floor);
    g.setColour(C_EDGE.withAlpha(0.65f));
    g.strokePath(floor, juce::PathStrokeType(1.5f));
}

//==============================================================================
//  drawWalls
//==============================================================================
void VBAPAudioProcessorEditor::drawWalls(juce::Graphics& g)
{
    auto FL = isoProj(-ROOM_X, 0.0f, 0.0f);
    auto FR = isoProj(ROOM_X, 0.0f, 0.0f);
    auto BL = isoProj(-ROOM_X, ROOM_Y, 0.0f);
    auto BR = isoProj(ROOM_X, ROOM_Y, 0.0f);
    auto FLT = isoProj(-ROOM_X, 0.0f, ROOM_Z);
    auto FRT = isoProj(ROOM_X, 0.0f, ROOM_Z);
    auto BLT = isoProj(-ROOM_X, ROOM_Y, ROOM_Z);
    auto BRT = isoProj(ROOM_X, ROOM_Y, ROOM_Z);

    auto drawGradWall = [&](juce::Point<float> bl, juce::Point<float> br,
        juce::Point<float> tr, juce::Point<float> tl,
        juce::Colour cBot, juce::Colour cTop)
        {
            juce::Path wall;
            wall.startNewSubPath(bl); wall.lineTo(br);
            wall.lineTo(tr); wall.lineTo(tl); wall.closeSubPath();
            juce::ColourGradient wg(cBot, bl.x, (bl.y + br.y) * 0.5f,
                cTop, tl.x, (tl.y + tr.y) * 0.5f, false);
            g.setGradientFill(wg);
            g.fillPath(wall);
            g.setColour(C_EDGE.withAlpha(0.50f));
            g.strokePath(wall, juce::PathStrokeType(1.5f));
        };

    drawGradWall(BL, BR, BRT, BLT, C_WALL_BOT, C_WALL_TOP);  // trasera
    drawGradWall(FL, BL, BLT, FLT, C_WALL_BOT.brighter(0.05f), C_WALL_TOP.brighter(0.05f));  // izq
    drawGradWall(FR, BR, BRT, FRT, C_WALL_BOT.brighter(0.10f), C_WALL_TOP.brighter(0.10f));  // der
}

//==============================================================================
//  drawCeilingEdges
//==============================================================================
void VBAPAudioProcessorEditor::drawCeilingEdges(juce::Graphics& g)
{
    auto FLT = isoProj(-ROOM_X, 0.0f, ROOM_Z);
    auto FRT = isoProj(ROOM_X, 0.0f, ROOM_Z);
    auto BLT = isoProj(-ROOM_X, ROOM_Y, ROOM_Z);
    auto BRT = isoProj(ROOM_X, ROOM_Y, ROOM_Z);
    auto FL = isoProj(-ROOM_X, 0.0f, 0.0f);
    auto FR = isoProj(ROOM_X, 0.0f, 0.0f);

    juce::Path ceiling;
    ceiling.startNewSubPath(FLT); ceiling.lineTo(FRT);
    ceiling.lineTo(BRT); ceiling.lineTo(BLT); ceiling.closeSubPath();
    g.setColour(C_BG.withAlpha(0.35f));
    g.fillPath(ceiling);

    auto drawDash = [&](juce::Point<float> p1, juce::Point<float> p2, float alpha)
        {
            float dx = p2.x - p1.x, dy = p2.y - p1.y;
            float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1.f) return;
            float ux = dx / len, uy = dy / len;
            g.setColour(C_EDGE.withAlpha(alpha));
            for (float t = 0;t + 5 < len;t += 12)
                g.drawLine(p1.x + ux * t, p1.y + uy * t, p1.x + ux * (t + 5), p1.y + uy * (t + 5), 1.2f);
        };
    drawDash(FLT, FRT, 0.50f); drawDash(FRT, BRT, 0.50f);
    drawDash(BRT, BLT, 0.50f); drawDash(BLT, FLT, 0.50f);

    // Pilares delanteros
    g.setColour(C_EDGE.withAlpha(0.40f));
    g.drawLine(FL.x, FL.y, FLT.x, FLT.y, 1.5f);
    g.drawLine(FR.x, FR.y, FRT.x, FRT.y, 1.5f);
}

//==============================================================================
//  drawGrids
//==============================================================================
void VBAPAudioProcessorEditor::drawGrids(juce::Graphics& g)
{
    g.setColour(C_GRID.withAlpha(0.25f));
    const int RX = (int)ROOM_X, RY = (int)ROOM_Y, RZ = (int)ROOM_Z;

    // Piso: lineas paralelas al eje Y (1m de paso)
    for (int ix = -RX; ix <= RX; ++ix) {
        auto p1 = isoProj((float)ix, 0.0f, 0.0f);
        auto p2 = isoProj((float)ix, ROOM_Y, 0.0f);
        g.drawLine(p1.x, p1.y, p2.x, p2.y, (ix == 0) ? 1.2f : 0.6f);
    }
    // Piso: lineas paralelas al eje X
    for (int iy = 0; iy <= RY; ++iy) {
        auto p1 = isoProj(-ROOM_X, (float)iy, 0.0f);
        auto p2 = isoProj(ROOM_X, (float)iy, 0.0f);
        g.drawLine(p1.x, p1.y, p2.x, p2.y, (iy == 0 || iy == RY) ? 1.0f : 0.55f);
    }

    g.setColour(C_GRID.withAlpha(0.16f));

    // Pared trasera
    for (int ix = -RX; ix <= RX; ++ix) {
        auto p1 = isoProj((float)ix, ROOM_Y, 0.0f);
        auto p2 = isoProj((float)ix, ROOM_Y, ROOM_Z);
        g.drawLine(p1.x, p1.y, p2.x, p2.y, 0.6f);
    }
    for (int iz = 0; iz <= RZ; ++iz) {
        auto p1 = isoProj(-ROOM_X, ROOM_Y, (float)iz);
        auto p2 = isoProj(ROOM_X, ROOM_Y, (float)iz);
        g.drawLine(p1.x, p1.y, p2.x, p2.y, 0.6f);
    }

    // Pared izquierda
    for (int iy = 0; iy <= RY; ++iy) {
        auto p1 = isoProj(-ROOM_X, (float)iy, 0.0f);
        auto p2 = isoProj(-ROOM_X, (float)iy, ROOM_Z);
        g.drawLine(p1.x, p1.y, p2.x, p2.y, 0.5f);
    }
    for (int iz = 0; iz <= RZ; ++iz) {
        auto p1 = isoProj(-ROOM_X, 0.0f, (float)iz);
        auto p2 = isoProj(-ROOM_X, ROOM_Y, (float)iz);
        g.drawLine(p1.x, p1.y, p2.x, p2.y, 0.5f);
    }

    // Pared derecha
    for (int iy = 0; iy <= RY; ++iy) {
        auto p1 = isoProj(ROOM_X, (float)iy, 0.0f);
        auto p2 = isoProj(ROOM_X, (float)iy, ROOM_Z);
        g.drawLine(p1.x, p1.y, p2.x, p2.y, 0.5f);
    }

    // Etiquetas de escala en el piso (cada 1m)
    g.setColour(C_TEXT.withAlpha(0.18f));
    g.setFont(juce::Font(juce::FontOptions().withHeight(9.5f)));
    for (int iy = 1; iy <= RY; ++iy) {
        auto p = isoProj(ROOM_X, (float)iy, 0.0f);
        g.drawText(juce::String(iy) + "m", (int)p.x + 4, (int)p.y - 6, 30, 12, juce::Justification::left);
    }
}

//==============================================================================
//  drawDistanceRings
//==============================================================================
void VBAPAudioProcessorEditor::drawDistanceRings(juce::Graphics& g)
{
    // Anillos en el piso a 1m, 2m, 3m del oyente (proyectados al piso).
    const int steps = 48;
    for (int d = 1; d <= (int)ROOM_Y; ++d)
    {
        bool major = (d == (int)ROOM_Y);   // anillo mayor en el borde del cuarto
        juce::Path ring;
        bool first = true;
        for (int s = 0; s <= steps; ++s)
        {
            float ang = juce::MathConstants<float>::twoPi * s / steps
                - juce::MathConstants<float>::halfPi;
            float rx2 = std::cos(ang) * (float)d;
            float ry2 = std::sin(ang) * (float)d + (float)d;
            float wx = juce::jlimit(-ROOM_X, ROOM_X, rx2);
            float wy = juce::jlimit(0.0f, ROOM_Y, ry2);
            auto pt = isoProj(wx, wy, 0.02f);
            if (first) { ring.startNewSubPath(pt); first = false; }
            else ring.lineTo(pt);
        }
        g.setColour(C_RING.withAlpha(major ? 0.35f : 0.18f));
        g.strokePath(ring, juce::PathStrokeType(major ? 1.0f : 0.55f));

        if (major) {
            auto lbl = isoProj((float)d * 0.75f, (float)d, 0.1f);
            g.setColour(C_TEXT.withAlpha(0.28f));
            g.setFont(juce::Font(juce::FontOptions().withHeight(9.5f)));
            g.drawText(juce::String(d) + "m", (int)lbl.x, (int)lbl.y - 6, 30, 12, juce::Justification::left);
        }
    }
}

//==============================================================================
//  drawEnergyLines
//==============================================================================
void VBAPAudioProcessorEditor::drawEnergyLines(juce::Graphics& g,
    juce::Point<float> srcPt, const juce::Point<float> spkPts[3])
{
    for (int i = 0;i < 3;++i)
    {
        float gv = gains[i], ag = std::abs(gv);
        if (ag < 0.015f) continue;
        juce::Colour lc = (gv < 0.f) ? C_NEG : C_SRC;
        float lw = 1.5f + 9.0f * ag;

        g.setColour(lc.withAlpha(0.04f + 0.05f * ag));
        g.drawLine(srcPt.x, srcPt.y, spkPts[i].x, spkPts[i].y, lw * 5.0f);
        g.setColour(lc.withAlpha(0.10f + 0.12f * ag));
        g.drawLine(srcPt.x, srcPt.y, spkPts[i].x, spkPts[i].y, lw * 2.0f);
        g.setColour(lc.withAlpha(0.25f + 0.35f * ag));
        g.drawLine(srcPt.x, srcPt.y, spkPts[i].x, spkPts[i].y, lw * 0.9f);
        g.setColour(lc.withAlpha(0.70f + 0.30f * ag));
        g.drawLine(srcPt.x, srcPt.y, spkPts[i].x, spkPts[i].y, lw * 0.35f);
    }
}

//==============================================================================
//  drawSpeaker3D
//==============================================================================
void VBAPAudioProcessorEditor::drawSpeaker3D(juce::Graphics& g,
    juce::Point<float> pos, const juce::String& label, float gainVal,
    bool dominant, bool mouseHovered)
{
    float ag = std::abs(gainVal);
    juce::Colour col = (gainVal < 0.f) ? C_NEG : C_SPK;

    for (int r = 6;r >= 1;--r) {
        float rv = 10.f + r * 9.f * ag;
        g.setColour(col.withAlpha(0.035f * r * ag));
        g.fillEllipse(pos.x - rv, pos.y - rv, rv * 2, rv * 2);
    }

    float bodyR = 11.f + ag * 4.f;

    if (dominant) {
        const float hR = bodyR + 6.f;
        g.setColour(C_SRC.withAlpha(0.45f));
        g.drawEllipse(pos.x - hR, pos.y - hR, hR * 2, hR * 2, 2.0f);
    }
    if (mouseHovered) {
        const float oR = bodyR + 4.f;
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.drawEllipse(pos.x - oR, pos.y - oR, oR * 2, oR * 2, 1.5f);
    }
    juce::ColourGradient grad(col.brighter(0.4f), pos.x - bodyR * .5f, pos.y - bodyR * .5f,
        col.darker(0.3f), pos.x + bodyR * .5f, pos.y + bodyR * .5f, true);
    g.setGradientFill(grad);
    g.fillEllipse(pos.x - bodyR, pos.y - bodyR, bodyR * 2, bodyR * 2);

    g.setColour(col.brighter(0.55f).withAlpha(0.65f));
    g.drawEllipse(pos.x - bodyR, pos.y - bodyR, bodyR * 2, bodyR * 2, 1.8f);

    // ---- Icono: monitor de estudio (cabinet + tweeter + woofer) ----
    {
        float iW = bodyR * 0.88f;   // ancho del cabinet
        float iH = bodyR * 1.20f;   // alto del cabinet
        float iX = pos.x - iW * 0.5f;
        float iY = pos.y - iH * 0.5f;

        // Cabinet (cuerpo del monitor)
        g.setColour(juce::Colours::black.withAlpha(0.45f));
        g.fillRoundedRectangle(iX, iY, iW, iH, iW * 0.12f);
        g.setColour(juce::Colours::white.withAlpha(0.38f));
        g.drawRoundedRectangle(iX, iY, iW, iH, iW * 0.12f, 1.1f);

        // Tweeter (pequeno, parte superior)
        float twR = iW * 0.16f;
        float twCX = pos.x;
        float twCY = iY + iH * 0.22f;
        g.setColour(juce::Colours::black.withAlpha(0.55f));
        g.fillEllipse(twCX - twR, twCY - twR, twR * 2, twR * 2);
        g.setColour(col.brighter(0.5f).withAlpha(0.70f));
        g.drawEllipse(twCX - twR, twCY - twR, twR * 2, twR * 2, 0.9f);
        // Punto central del tweeter
        g.setColour(juce::Colours::white.withAlpha(0.55f));
        g.fillEllipse(twCX - twR * 0.40f, twCY - twR * 0.40f, twR * 0.80f, twR * 0.80f);

        // Woofer (grande, parte inferior)
        float wfR = iW * 0.34f;
        float wfCX = pos.x;
        float wfCY = iY + iH * 0.65f;
        // Surround (aro exterior)
        g.setColour(juce::Colours::black.withAlpha(0.50f));
        g.fillEllipse(wfCX - wfR, wfCY - wfR, wfR * 2, wfR * 2);
        g.setColour(col.brighter(0.3f).withAlpha(0.60f));
        g.drawEllipse(wfCX - wfR, wfCY - wfR, wfR * 2, wfR * 2, 1.1f);
        // Cono del woofer
        float coneR = wfR * 0.72f;
        g.setColour(juce::Colour(40u, 50u, 70u).withAlpha(0.85f));
        g.fillEllipse(wfCX - coneR, wfCY - coneR, coneR * 2, coneR * 2);
        g.setColour(col.withAlpha(0.40f));
        g.drawEllipse(wfCX - coneR, wfCY - coneR, coneR * 2, coneR * 2, 0.8f);
        // Dust cap (tapa central)
        float capR = coneR * 0.38f;
        g.setColour(col.brighter(0.4f).withAlpha(0.65f));
        g.fillEllipse(wfCX - capR, wfCY - capR, capR * 2, capR * 2);
    }

    g.setColour(C_TEXT);
    g.setFont(juce::Font(juce::FontOptions().withHeight(11.f).withStyle("Bold")));
    g.drawText(label, (int)(pos.x + bodyR + 5), (int)(pos.y - 10), 22, 14, juce::Justification::left);
    g.setFont(juce::Font(juce::FontOptions().withHeight(10.f)));
    g.setColour(ag > 0.05f ? col.brighter(0.2f) : C_TEXT.withAlpha(0.38f));
    g.drawText(juce::String(gainVal, 3), (int)(pos.x + bodyR + 5), (int)(pos.y + 4), 46, 12, juce::Justification::left);
}

//==============================================================================
//  drawSourcePoint
//==============================================================================
void VBAPAudioProcessorEditor::drawSourcePoint(juce::Graphics& g,
    juce::Point<float> srcPt, float sx, float sy, float /*sz*/)
{
    auto shadow = isoProj(sx, sy, 0.f);
    g.setColour(C_SRC.withAlpha(0.20f));
    g.fillEllipse(shadow.x - 12, shadow.y - 4, 24, 8);

    // Poste discontinuo del piso a la fuente (clave perceptiva de altura).
    juce::Path pole;
    pole.startNewSubPath(shadow.x, shadow.y);
    pole.lineTo(srcPt.x, srcPt.y);
    juce::Path dashed;
    const float dashes[] = { 3.0f, 4.0f };
    juce::PathStrokeType(1.2f).createDashedStroke(dashed, pole, dashes, 2);
    g.setColour(C_SRC.withAlpha(0.35f));
    g.fillPath(dashed);

    bool hov = (dragMode == DragMode::None && hovering) || dragMode == DragMode::MoveSrc;
    float outerR = hov ? 34.f : 26.f;
    g.setColour(C_SRC.withAlpha(hov ? 0.30f : 0.14f));
    g.fillEllipse(srcPt.x - outerR, srcPt.y - outerR, outerR * 2, outerR * 2);
    g.setColour(C_SRC.withAlpha(0.45f));
    g.fillEllipse(srcPt.x - 18.f, srcPt.y - 18.f, 36.f, 36.f);

    juce::ColourGradient sg(C_SRC.brighter(0.55f), srcPt.x - 5, srcPt.y - 5,
        C_SRC.darker(0.40f), srcPt.x + 7, srcPt.y + 7, true);
    g.setGradientFill(sg);
    g.fillEllipse(srcPt.x - 13.f, srcPt.y - 13.f, 26.f, 26.f);

    g.setColour(juce::Colours::white.withAlpha(0.65f));
    g.fillEllipse(srcPt.x - 7.f, srcPt.y - 9.f, 7.f, 5.f);

    g.setColour(juce::Colours::white.withAlpha(0.70f));
    g.drawLine(srcPt.x - 7, srcPt.y, srcPt.x + 7, srcPt.y, 1.5f);
    g.drawLine(srcPt.x, srcPt.y - 7, srcPt.x, srcPt.y + 7, 1.5f);

    g.setFont(juce::Font(juce::FontOptions().withHeight(11.f).withStyle("Bold")));
    g.setColour(C_SRC.brighter(0.3f));
    g.drawText("Src", (int)srcPt.x + 16, (int)srcPt.y - 8, 32, 14, juce::Justification::left);
}

//==============================================================================
//  drawAxesAndOverlay
//==============================================================================
void VBAPAudioProcessorEditor::drawAxesAndOverlay(juce::Graphics& g)
{
    auto o = isoProj(0.f, 0.f, 0.f);
    const float axL = 1.0f;   // longitud de las flechas de eje (m)

    // Eje X (rojo)
    auto ex = isoProj(axL, 0.f, 0.f);
    g.setColour(juce::Colours::tomato.withAlpha(0.55f));
    g.drawArrow({ o.x,o.y,ex.x,ex.y }, 1.5f, 7.f, 6.f);
    g.setFont(juce::Font(juce::FontOptions().withHeight(10.f))); g.drawText("X", (int)ex.x - 4, (int)ex.y - 8, 12, 12, juce::Justification::centred);

    // Eje Y (verde)
    auto ey = isoProj(0.f, axL, 0.f);
    g.setColour(juce::Colours::limegreen.withAlpha(0.55f));
    g.drawArrow({ o.x,o.y,ey.x,ey.y }, 1.5f, 7.f, 6.f);
    g.drawText("Y", (int)ey.x - 6, (int)ey.y - 8, 12, 12, juce::Justification::centred);

    // Eje Z (azul)
    auto ez = isoProj(0.f, 0.f, axL);
    g.setColour(juce::Colours::dodgerblue.withAlpha(0.55f));
    g.drawArrow({ o.x,o.y,ez.x,ez.y }, 1.5f, 7.f, 6.f);
    g.drawText("Z", (int)ez.x - 4, (int)ez.y - 12, 12, 12, juce::Justification::centred);

    // ---- Flechas de IZQUIERDA / DERECHA en el piso ----
    // Estas flechas muestran claramente la convencion de ejes para el oyente.
    // IZQUIERDA del oyente = X negativo, DERECHA = X positivo.
    // Si los altavoces suenan al reves, activar "Intercambiar L/R" en el panel.
    {
        bool swapped = pSwapLR->load() > 0.5f;

        // Flecha hacia la IZQUIERDA (X negativo) — escalada al cuarto ~3m
        auto arrowL_tip = isoProj(-2.0f, 0.6f, 0.05f);
        auto arrowL_base = isoProj(-0.8f, 0.6f, 0.05f);
        juce::Colour colL = swapped ? juce::Colour(255u, 200u, 100u) : juce::Colour(255u, 182u, 193u);
        g.setColour(colL.withAlpha(0.85f));
        g.drawArrow({ arrowL_base.x, arrowL_base.y, arrowL_tip.x, arrowL_tip.y }, 2.5f, 10.f, 8.f);
        // Etiqueta
        auto lblL = isoProj(-2.3f, 0.6f, 0.3f);
        g.setFont(juce::Font(juce::FontOptions().withHeight(11.f).withStyle("Bold")));
        g.drawText(swapped ? "DERECHA" : "IZQUIERDA", (int)lblL.x - 40, (int)lblL.y - 8, 80, 14, juce::Justification::centred);
        g.setFont(juce::Font(juce::FontOptions().withHeight(9.5f)));
        g.setColour(colL.withAlpha(0.60f));
        g.drawText(swapped ? "(CH2)" : "(CH1)", (int)lblL.x - 20, (int)lblL.y + 5, 40, 12, juce::Justification::centred);

        // Flecha hacia la DERECHA (X positivo)
        auto arrowR_tip = isoProj(2.0f, 0.6f, 0.05f);
        auto arrowR_base = isoProj(0.8f, 0.6f, 0.05f);
        juce::Colour colR = swapped ? juce::Colour(255u, 182u, 193u) : juce::Colour(255u, 200u, 100u);
        g.setColour(colR.withAlpha(0.85f));
        g.drawArrow({ arrowR_base.x, arrowR_base.y, arrowR_tip.x, arrowR_tip.y }, 2.5f, 10.f, 8.f);
        auto lblR = isoProj(2.3f, 0.6f, 0.3f);
        g.setFont(juce::Font(juce::FontOptions().withHeight(11.f).withStyle("Bold")));
        g.setColour(colR.withAlpha(0.85f));
        g.drawText(swapped ? "IZQUIERDA" : "DERECHA", (int)lblR.x - 40, (int)lblR.y - 8, 80, 14, juce::Justification::centred);
        g.setFont(juce::Font(juce::FontOptions().withHeight(9.5f)));
        g.setColour(colR.withAlpha(0.60f));
        g.drawText(swapped ? "(CH1)" : "(CH2)", (int)lblR.x - 20, (int)lblR.y + 5, 40, 12, juce::Justification::centred);

        // Si swap activo: advertencia visual
        if (swapped)
        {
            int wx = isoArea.getRight() - 160, wy = isoArea.getY() + 28;
            g.setColour(juce::Colour(255u, 200u, 100u).withAlpha(0.20f));
            g.fillRoundedRectangle((float)wx, (float)wy, 148, 18, 5);
            g.setFont(juce::Font(juce::FontOptions().withHeight(10.f).withStyle("Bold")));
            g.setColour(juce::Colour(255u, 200u, 100u).withAlpha(0.90f));
            g.drawText("L/R INTERCAMBIADOS", wx, wy, 148, 18, juce::Justification::centred);
        }
    }

    // (El arco de azimut fue removido — con mapeo lineal az no es un angulo
    //  en sentido espacial, sino la posicion lateral en grados-equivalentes.)

    // Oyente: posicion real en (0, 0, kListenerZ) (no en el piso).
    // El poste discontinuo desde el piso lo hace visible verticalmente.
    auto oyentePt   = isoProj(0.f, 0.f, VBAPAudioProcessor::kListenerZ);
    auto oyenteFoot = isoProj(0.f, 0.f, 0.f);

    // Sombra en el piso
    g.setColour(juce::Colours::white.withAlpha(0.10f));
    g.fillEllipse(oyenteFoot.x - 10, oyenteFoot.y - 3, 20, 6);

    // Poste discontinuo del piso al oyente
    {
        juce::Path pole;
        pole.startNewSubPath(oyenteFoot.x, oyenteFoot.y);
        pole.lineTo(oyentePt.x, oyentePt.y);
        juce::Path dashed;
        const float dashes[] = { 3.0f, 4.0f };
        juce::PathStrokeType(1.0f).createDashedStroke(dashed, pole, dashes, 2);
        g.setColour(juce::Colours::white.withAlpha(0.20f));
        g.fillPath(dashed);
    }

    g.setColour(juce::Colours::white.withAlpha(0.15f));
    g.fillEllipse(oyentePt.x - 18, oyentePt.y - 18, 36, 36);
    g.setColour(juce::Colours::white.withAlpha(0.75f));
    g.drawEllipse(oyentePt.x - 9, oyentePt.y - 9, 18, 18, 2.f);
    g.setColour(juce::Colours::white);
    g.fillEllipse(oyentePt.x - 3, oyentePt.y - 3, 6, 6);
    g.setFont(juce::Font(juce::FontOptions().withHeight(10.5f)));
    g.setColour(C_TEXT.withAlpha(0.50f));
    g.drawText("Oyente", (int)oyentePt.x + 12, (int)oyentePt.y - 7, 54, 14, juce::Justification::left);

    // Readout Az/El/Dist en la esquina inferior. D = distancia euclidea real
    // al sweet spot (no es el valor del slider, que es lineal en Y). Esto le
    // da al usuario el feedback "fisico" sin acoplar el slider.
    {
        float az = (float)azSlider.getValue();
        float el = (float)elSlider.getValue();
        float vsx = pSrcX->load();
        float vsy = pSrcY->load();
        float vsz = pSrcZ->load();
        const auto& sp = audioProcessor.getSweetSpot();
        const float dEuclid = std::sqrt((vsx - sp.x()) * (vsx - sp.x())
                                       + (vsy - sp.y()) * (vsy - sp.y())
                                       + (vsz - sp.z()) * (vsz - sp.z()));

        int bx = isoArea.getX() + 12, by = isoArea.getBottom() - 44;
        g.setColour(C_BG.withAlpha(0.70f));
        g.fillRoundedRectangle((float)bx - 6, (float)by - 4, 330, 40, 5);
        g.setFont(juce::Font(juce::FontOptions().withHeight(11.f).withStyle("Bold")));
        g.setColour(C_AZ.withAlpha(0.90f));
        g.drawText("Az: " + juce::String(az, 1) + " deg    El: " + juce::String(el, 1) + " deg    D: " + juce::String(dEuclid, 2) + " m",
            bx, by, 320, 18, juce::Justification::left);
        g.setFont(juce::Font(juce::FontOptions().withHeight(10.f)));
        g.setColour(C_TEXT.withAlpha(0.38f));
        g.drawText("X: " + juce::String(vsx, 2) + "  Y: " + juce::String(vsy, 2) + "  Z: " + juce::String(vsz, 2) + " m",
            bx, by + 18, 280, 14, juce::Justification::left);
    }
}

//==============================================================================
//  drawCameraHint  —  indicador de camara orbital en la esquina superior
//==============================================================================
void VBAPAudioProcessorEditor::drawCameraHint(juce::Graphics& g)
{
    int hx = isoArea.getRight() - 200, hy = isoArea.getY() + 8;

    // Fondo del indicador
    g.setColour(C_BG.withAlpha(0.65f));
    g.fillRoundedRectangle((float)hx - 6, (float)hy - 4, 194, 44, 5);

    // Icono de camara (simple circulo con lente)
    bool rotating = (dragMode == DragMode::RotateCamera);
    g.setColour(C_CAM_HINT.withAlpha(rotating ? 1.0f : 0.55f));
    g.fillEllipse((float)hx, (float)hy + 4, 18, 18);
    g.setColour(C_BG.withAlpha(0.8f));
    g.fillEllipse((float)hx + 4, (float)hy + 8, 10, 10);
    g.setColour(C_CAM_HINT.withAlpha(rotating ? 1.0f : 0.55f));
    g.fillEllipse((float)hx + 6, (float)hy + 10, 6, 6);

    // Texto de pista
    g.setFont(rotating ? juce::Font(juce::FontOptions().withHeight(10.f).withStyle("Bold"))
        : juce::Font(juce::FontOptions().withHeight(10.f)));
    g.setColour(C_CAM_HINT.withAlpha(rotating ? 0.95f : 0.55f));
    g.drawText("Arrastrar: rotar camara", hx + 24, hy + 2, 170, 13, juce::Justification::left);
    g.setColour(C_TEXT.withAlpha(0.35f));
    g.setFont(juce::Font(juce::FontOptions().withHeight(9.5f)));
    g.drawText("Doble-click: reset vista", hx + 24, hy + 16, 170, 12, juce::Justification::left);

    // Mostrar angulos actuales de camara
    g.setColour(C_CAM_HINT.withAlpha(0.45f));
    g.drawText("Az:" + juce::String((int)camAzimuth) + "  El:" + juce::String((int)camElevation),
        hx + 24, hy + 28, 170, 12, juce::Justification::left);
}

//==============================================================================
//  drawRoom3D  —  orquesta todo el dibujo
//==============================================================================
void VBAPAudioProcessorEditor::drawRoom3D(juce::Graphics& g)
{
    g.setColour(C_PANEL);
    g.fillRoundedRectangle(isoArea.toFloat(), 10.f);
    g.setColour(C_EDGE.withAlpha(0.35f));
    g.drawRoundedRectangle(isoArea.toFloat(), 10.f, 1.5f);

    g.saveState();
    g.reduceClipRegion(isoArea.reduced(3));

    // Superficies del cuarto (orden de pintor)
    drawWalls(g);
    drawFloor(g);
    drawGrids(g);
    drawDistanceRings(g);
    drawCeilingEdges(g);

    // Objetos en el espacio
    float sx = pSrcX->load();
    float sy = pSrcY->load();
    float sz = pSrcZ->load();
    auto srcPt = isoProj(sx, sy, sz);

    auto& spk = audioProcessor.getSpeakerPositions();
    const char* lbls[] = { "L","R","C" };
    juce::Point<float> spkPts[3];
    for (int i = 0;i < 3;++i) spkPts[i] = isoProj(spk(i, 0), spk(i, 1), spk(i, 2));

    drawEnergyLines(g, srcPt, spkPts);

    // Identificar el altavoz dominante (mayor |gain|) si supera un piso minimo.
    dominantSpeakerIdx = -1;
    float maxAg = 0.05f;
    for (int i = 0;i < 3;++i) {
        float a = std::abs(gains[i]);
        if (a > maxAg) { maxAg = a; dominantSpeakerIdx = i; }
    }
    for (int i = 0;i < 3;++i)
        drawSpeaker3D(g, spkPts[i], lbls[i], gains[i],
            i == dominantSpeakerIdx, i == hoverSpeakerIdx);

    drawSourcePoint(g, srcPt, sx, sy, sz);
    drawAxesAndOverlay(g);
    drawCameraHint(g);

    // Lectura X/Y/Z flotante junto al cursor mientras se arrastra la fuente.
    if (dragMode == DragMode::MoveSrc) {
        const float bw = 110.f, bh = 22.f;
        float bx = dragCursorPos.x + 14.f;
        float by = dragCursorPos.y - 28.f;
        bx = juce::jlimit((float)isoArea.getX() + 4.f,
                          (float)isoArea.getRight() - bw - 4.f, bx);
        by = juce::jlimit((float)isoArea.getY() + 4.f,
                          (float)isoArea.getBottom() - bh - 4.f, by);
        g.setColour(C_BG.withAlpha(0.85f));
        g.fillRoundedRectangle(bx, by, bw, bh, 4.f);
        g.setColour(C_SRC.withAlpha(0.55f));
        g.drawRoundedRectangle(bx, by, bw, bh, 4.f, 1.f);
        g.setFont(juce::Font(juce::FontOptions().withHeight(11.f).withStyle("Bold")));
        g.setColour(C_SRC.brighter(0.2f));
        g.drawText("X " + juce::String(sx, 2) + "  Y " + juce::String(sy, 2)
                   + "  Z " + juce::String(sz, 2),
            juce::Rectangle<float>(bx, by, bw, bh), juce::Justification::centred);
    }

    g.restoreState();
}

//==============================================================================
//  drawGainBars  -  barras pastel en el panel derecho (gainArea)
//==============================================================================
void VBAPAudioProcessorEditor::drawGainBars(juce::Graphics& g)
{
    if (gainArea.isEmpty() || gainArea.getHeight() < 40) return;

    static const juce::Colour PASTEL_L{ 255u, 182u, 193u };
    static const juce::Colour PASTEL_R{ 173u, 216u, 230u };
    static const juce::Colour PASTEL_C{ 180u, 235u, 180u };
    static const juce::Colour PASTEL_NEG{ 255u, 200u, 150u };
    static const juce::Colour PASTELS[3] = { PASTEL_L, PASTEL_R, PASTEL_C };

    bool swapLR = pSwapLR->load() > 0.5f;
    const char* labNorm[3] = { "L (CH1)",  "R (CH2)", "C (CH3)" };
    const char* labSwap[3] = { "R (CH1)",  "L (CH2)", "C (CH3)" };
    const char** lbl = swapLR ? labSwap : labNorm;

    const int W = gainArea.getWidth();
    const int H = gainArea.getHeight();
    const int tH = 18, vH = 13, lH = 14;
    const int bH = H - tH - vH - lH - 6;
    const int bW = (W - 28) / 3;

    g.setColour(juce::Colour(22u, 34u, 52u));
    g.fillRoundedRectangle(gainArea.toFloat(), 6.f);
    g.setColour(juce::Colour(60u, 90u, 140u).withAlpha(0.40f));
    g.drawRoundedRectangle(gainArea.toFloat(), 6.f, 1.f);

    g.setFont(juce::Font(juce::FontOptions().withHeight(10.f).withStyle("Bold")));
    g.setColour(juce::Colour(180u, 210u, 255u));
    g.drawText("Ganancias VBAP",
        gainArea.getX(), gainArea.getY() + 2, W, tH, juce::Justification::centred);

    // Escala dB con tope en 0: cualquier ganancia >= 1.0 lineal se muestra
    // como 0 dB; debajo de eso es negativo. Floor -24 dB coincide con
    // kMinGainDb de la atenuacion 1/r, asi que el rango visible se mapea 1:1
    // a la atenuacion alcanzable.
    constexpr float kFloorDb = -24.0f;
    for (int i = 0; i < 3; ++i)
    {
        int bx = gainArea.getX() + 8 + i * (bW + 6);
        int by = gainArea.getY() + tH + vH;
        float gv = gains[i];
        float dB = juce::jmin(0.0f, juce::Decibels::gainToDecibels(std::abs(gv), kFloorDb));
        float ag = juce::jlimit(0.f, 1.f, (dB - kFloorDb) / -kFloorDb);
        int   fH = (int)(bH * ag);
        juce::Colour p = (gv < 0.f) ? PASTEL_NEG : PASTELS[i];

        // Valor numerico en dB (0 dB = tope, negativos abajo).
        g.setColour(p.withAlpha(0.16f));
        g.fillRoundedRectangle((float)bx, (float)(gainArea.getY() + tH), (float)bW, (float)vH, 3.f);
        g.setFont(juce::Font(juce::FontOptions().withHeight(9.f).withStyle("Bold")));
        g.setColour(p.brighter(0.1f));
        g.drawText(juce::String(dB, 1) + " dB", bx, gainArea.getY() + tH, bW, vH, juce::Justification::centred);

        // Barra
        g.setColour(juce::Colour(12u, 20u, 34u).withAlpha(0.90f));
        g.fillRoundedRectangle((float)bx, (float)by, (float)bW, (float)bH, 3.f);
        if (fH > 1) {
            int barTop = by + bH - fH;
            juce::ColourGradient gr(p.brighter(0.15f), (float)bx, (float)barTop,
                p.darker(0.10f), (float)bx, (float)(barTop + fH), false);
            g.setGradientFill(gr);
            g.fillRoundedRectangle((float)bx, (float)barTop, (float)bW, (float)fH, 3.f);
            g.setColour(juce::Colours::white.withAlpha(0.28f));
            g.fillRoundedRectangle((float)bx, (float)barTop, (float)bW, std::min(3.f, (float)fH), 2.f);
        }
        g.setColour(juce::Colour(50u, 75u, 110u).withAlpha(0.50f));
        g.drawRoundedRectangle((float)bx, (float)by, (float)bW, (float)bH, 3.f, 1.f);

        // Etiqueta canal
        g.setColour(p.withAlpha(0.18f));
        g.fillRoundedRectangle((float)bx, (float)(by + bH + 3), (float)bW, (float)lH, 3.f);
        g.setFont(juce::Font(juce::FontOptions().withHeight(9.5f).withStyle("Bold")));
        g.setColour(p.brighter(0.05f));
        g.drawText(lbl[i], bx, by + bH + 3, bW, lH, juce::Justification::centred);
    }
}

//==============================================================================

//==============================================================================
//  paint()
//==============================================================================
void VBAPAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(C_BG);

    g.setFont(juce::Font(juce::FontOptions().withHeight(16.f).withStyle("Bold")));
    g.setColour(C_TEXT);
    g.drawFittedText("VBAP Spatializer", 0, 4, getWidth() - RIGHT_W, 24, juce::Justification::centred, 1);

    g.setColour(C_EDGE.withAlpha(0.55f));
    g.fillRect(getWidth() - RIGHT_W - 5, TITLE_H, 2, getHeight() - TITLE_H);

    // Labels panel derecho - pastillas con fondo claro para buena legibilidad
    const int rx = getWidth() - RIGHT_W - PAD, sy0 = TITLE_H + PAD, sw = RIGHT_W - PAD * 2;

    // Colores de cada label (coordinados con las barras de ganancia)
    struct LabelInfo { const char* text; juce::Colour pill; };
    LabelInfo lbls[] = {
        { "Azimut [deg]:",    juce::Colour(100u,220u,180u) },   // verde-azul (C_AZ)
        { "Elevacion [deg]:", juce::Colour(100u,200u,220u) },   // azul claro
        { "Distancia [m]:",   juce::Colour(160u,200u,255u) },   // lavanda
    };
    g.setFont(juce::Font(juce::FontOptions().withHeight(11.5f).withStyle("Bold")));
    for (int i = 0; i < 3; ++i)
    {
        int ly = sy0 + i * ROW_H;
        // Pastilla de fondo semitransparente
        g.setColour(lbls[i].pill.withAlpha(0.12f));
        g.fillRoundedRectangle((float)(rx + PAD - 4), (float)(ly - 2), (float)(sw + 8), (float)(LABEL_H + 4), 4.f);
        // Texto brillante y legible
        g.setColour(lbls[i].pill.brighter(0.1f));
        g.drawText(lbls[i].text, rx + PAD, ly, sw, LABEL_H, juce::Justification::left);
    }

    // (Antes habia una etiqueta "Delay Center [ms]:" encima del slider; con
    // el toggle el texto va en el propio boton, no se dibuja por separado.)

    drawRoom3D(g);
    drawGainBars(g);
}

//==============================================================================
//  MOUSE
//==============================================================================
void VBAPAudioProcessorEditor::mouseDown(const juce::MouseEvent& e)
{
    if (!isoArea.contains(e.getPosition())) return;

    float sx = pSrcX->load();
    float sy = pSrcY->load();
    float sz = pSrcZ->load();
    auto srcPt = isoProj(sx, sy, sz);

    if (srcPt.getDistanceFrom(e.position.toFloat()) < GRAB_R) {
        // Click cerca del Src -> mover fuente
        dragMode = DragMode::MoveSrc;
        zDragActive = e.mods.isShiftDown();
        lastDragPos = e.position.toFloat();
        dragCursorPos = e.position.toFloat();
    }
    else {
        // Click en espacio vacio -> rotar camara
        dragMode = DragMode::RotateCamera;
        lastDragPos = e.position.toFloat();
    }
    repaint();
}

void VBAPAudioProcessorEditor::mouseDrag(const juce::MouseEvent& e)
{
    if (dragMode == DragMode::MoveSrc)
    {
        auto pos = e.position.toFloat();
        dragCursorPos = pos;

        // Top-down: Z-drag mal definido (cos(el) ~ 0). Shift se ignora alli.
        const bool topView = camElevation > 80.0f;
        if (zDragActive && !topView)
        {
            const float dyPx = pos.y - lastDragPos.y;
            const float elRad = juce::degreesToRadians(camElevation);
            const float denom = std::max(std::abs(std::cos(elRad)), 0.10f);
            const float dz = -dyPx / (denom * camScale);   // arriba = +Z
            const float curZ = pSrcZ->load();
            const float curX = pSrcX->load();
            const float curY = pSrcY->load();
            const float newZ = juce::jlimit(VBAPAudioProcessor::kListenerZ,
                                            VBAPAudioProcessor::kSpeakerZMax,
                                            curZ + dz);
            setXYZParameters(curX, curY, newZ);
            lastDragPos = pos;
        }
        else
        {
            const float wz = pSrcZ->load();
            auto [wx, wy] = isoInverseXY(pos, wz);
            setXYZParameters(wx, wy, wz);
            lastDragPos = pos;
        }
    }
    else if (dragMode == DragMode::RotateCamera)
    {
        auto pos = e.position.toFloat();
        float dx = pos.x - lastDragPos.x;
        float dy = pos.y - lastDragPos.y;
        lastDragPos = pos;

        camAzimuth += dx * CAM_AZ_SENS;
        camElevation -= dy * CAM_EL_SENS;
        camElevation = juce::jlimit(5.0f, 85.0f, camElevation);
        // Mantener azimut en [0, 360)
        while (camAzimuth < 0.f) camAzimuth += 360.f;
        while (camAzimuth >= 360.f) camAzimuth -= 360.f;
        updateCameraTrig();
        // Rotacion manual rompe el "snap" a una preset: limpiar highlight.
        if (activePreset.has_value())
        {
            activePreset.reset();
            updatePresetButtonStates();
        }
    }
    repaint();
}

void VBAPAudioProcessorEditor::mouseUp(const juce::MouseEvent&)
{
    dragMode = DragMode::None;
    repaint();
}

void VBAPAudioProcessorEditor::mouseMove(const juce::MouseEvent& e)
{
    if (dragMode != DragMode::None) return;
    const auto mp = e.position.toFloat();

    float sx = pSrcX->load();
    float sy = pSrcY->load();
    float sz = pSrcZ->load();
    auto sp = isoProj(sx, sy, sz);

    const bool wasHov = hovering;
    hovering = (sp.getDistanceFrom(mp) < GRAB_R);

    int newHoverSpk = -1;
    if (!hovering) {
        const auto& spk = audioProcessor.getSpeakerPositions();
        float bestD = 18.0f;
        for (int i = 0; i < 3; ++i) {
            auto p = isoProj(spk(i, 0), spk(i, 1), spk(i, 2));
            float d = p.getDistanceFrom(mp);
            if (d < bestD) { bestD = d; newHoverSpk = i; }
        }
    }

    const bool changed = (hovering != wasHov) || (newHoverSpk != hoverSpeakerIdx);
    hoverSpeakerIdx = newHoverSpk;
    if (changed) {
        setMouseCursor(hovering ? juce::MouseCursor::DraggingHandCursor
            : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void VBAPAudioProcessorEditor::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (!isoArea.contains(e.getPosition())) return;
    // Doble-click en la vista -> reset camara
    camAzimuth = CAM_AZ_DEFAULT;
    camElevation = CAM_EL_DEFAULT;
    camScale = CAM_SCALE_DEFAULT;
    updateCameraTrig();
    repaint();
}

void VBAPAudioProcessorEditor::mouseWheelMove(const juce::MouseEvent& e,
    const juce::MouseWheelDetails& w)
{
    if (!isoArea.contains(e.getPosition())) return;
    const float dy = w.isReversed ? -w.deltaY : w.deltaY;
    camScale = juce::jlimit(40.0f, 400.0f, camScale * std::exp(dy * 0.20f));
    repaint();
}

bool VBAPAudioProcessorEditor::keyPressed(const juce::KeyPress& k)
{
    switch (k.getKeyCode())
    {
        case '1': applyPreset(CamPreset::Iso);   return true;
        case '2': applyPreset(CamPreset::Top);   return true;
        case '3': applyPreset(CamPreset::Front); return true;
        case '4': applyPreset(CamPreset::Side);  return true;
        default: return false;
    }
}

//==============================================================================
//  Timer
//==============================================================================
void VBAPAudioProcessorEditor::timerCallback()
{
    for (int i = 0;i < 3;++i)
        gains[i] = audioProcessor.getLastGain(i);

    const float x = pSrcX->load();
    const float y = pSrcY->load();
    const float z = pSrcZ->load();
    float az, el, dist;
    xyzToSpherical(x, y, z, az, el, dist);

    updatingFromTimer = true;
    azSlider.setValue(az, juce::dontSendNotification);
    elSlider.setValue(el, juce::dontSendNotification);
    distSlider.setValue(dist, juce::dontSendNotification);
    updatingFromTimer = false;

    // Repaint condicional: solo si algo cambio sensiblemente.
    // Tambien repinta si el mouse esta arrastrando (cursor/feedback animado).
    constexpr float kEpsPos  = 0.001f;
    constexpr float kEpsGain = 0.001f;
    const bool moved = std::abs(x - prevX) > kEpsPos
                    || std::abs(y - prevY) > kEpsPos
                    || std::abs(z - prevZ) > kEpsPos;
    const bool gainsChanged = std::abs(gains[0] - prevGains[0]) > kEpsGain
                           || std::abs(gains[1] - prevGains[1]) > kEpsGain
                           || std::abs(gains[2] - prevGains[2]) > kEpsGain;

    if (moved || gainsChanged || dragMode != DragMode::None)
    {
        prevX = x; prevY = y; prevZ = z;
        prevGains[0] = gains[0]; prevGains[1] = gains[1]; prevGains[2] = gains[2];
        repaint();
    }
}