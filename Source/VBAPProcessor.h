//
//  VBAPProcessor.h
//  VBAP
//
//  Created by Fernando Daza on 15/09/25.
//

#pragma once
#include <Eigen/Dense>
#include <array>
#include <vector>
#include <cmath>
#include <atomic>
#include <algorithm> // Added as per instructions

// VBAPProcessor3D
// - Diseñado para 3 altavoces fijos (columns in altPos: rows in MATLAB).
// - Real-time safe: no allocations en processBlock. Reservar buffers en prepare.
// - Implementa delays fraccionales por salida con interpolación lineal.
class VBAPProcessor
{
public:
    VBAPProcessor() = default;

    // Llamar en el hilo de configuración (prepareToPlay)
    // altPos: 3x3 matrix (rows = speakers, cols = x,y,z) like MATLAB altPos
    void setSpeakerPositions(const Eigen::Matrix<float,3,3>& altPosRows)
    {
        // Normalizar cada fila como vector unitario (desde el oyente en 0,0,0)
        Eigen::Matrix<float,3,3> normalizedRows;
        for (int i = 0; i < 3; ++i)
        {
            Eigen::Vector3f v = altPosRows.row(i).transpose();
            float n = v.norm();
            if (n > 1e-9f)
                v /= n; // convertir a unitario
            else
                v.setZero();
            normalizedRows.row(i) = v.transpose();
        }

        // Transponer a columnas (lo que espera el cálculo de A y Ginv)
        Eigen::Matrix<float,3,3> cols;
        cols.col(0) = normalizedRows.row(0).transpose(); // L
        cols.col(1) = normalizedRows.row(1).transpose(); // T
        cols.col(2) = normalizedRows.row(2).transpose(); // R

        speakerVecs = cols;
        A = speakerVecs;

        // check determinant first, avoid calling inverse() on singular matrix
        float det = A.determinant();
        if (std::isnan(det) || std::abs(det) < 1e-9f)
        {
            inverseValid = false;
            return;
        }

        // cheap condition number estimate
        Eigen::Matrix<float,3,3> invA = A.inverse();
        float condA = A.norm() * invA.norm();
        if (std::isnan(condA) || condA > 1e12f)
        {
            inverseValid = false;
        }
        else
        {
            Ginv = invA;
            inverseValid = true;
        }
    }

    // configuración que se llama en prepareToPlay
    void setSampleRate(double fs, int maxBlockSamples, float maxDistanceMeters = 10.0f, float speedOfSound_m_s = 343.0f)
    {
        sampleRate = (fs > 0.0) ? fs : 44100.0;
        maxBlock = std::max(1, maxBlockSamples);
        c = (speedOfSound_m_s > 0.0f) ? speedOfSound_m_s : 343.0f;

        float maxDelaySec = std::max(0.001f, maxDistanceMeters / c);
        maxDelaySamples = static_cast<int>(std::ceil(maxDelaySec * static_cast<float>(sampleRate))) + maxBlock + 8;
        if (maxDelaySamples < 16) maxDelaySamples = 16;

        for (int i=0;i<3;++i)
        {
            delayBuf[i].assign(static_cast<size_t>(maxDelaySamples), 0.0f);
            writeIndex[i] = 0;
        }
    }
    
    // debug getters (read-only, inline y sin costo en RT)
    const Eigen::Matrix<float,3,3>& getSpeakerMatrix() const noexcept { return speakerVecs; }
    const Eigen::Matrix<float,3,3>& getGinv() const noexcept { return Ginv; }

    // compute gains g for source direction s (az, el in degrees)
    // same logic as MATLAB: solve A * g = sUnit => clamp negatives => normalize energy
    Eigen::Vector3f computeGainsDeg(float azDeg, float elDeg) const
    {
        Eigen::Vector3f s = directionVectorDeg(azDeg, elDeg);

        Eigen::Vector3f g;
        if (inverseValid)
            g = Ginv * s;
        else
        {
            // fallback cosine-like
            for (int i=0;i<3;++i)
                g(i) = std::max(0.0f, speakerVecs.col(i).dot(s));
        }

        // Reasignar L/R con paneo equal-power en función del azimuth para suavizar la transición
        // Convención esperada: az=-90 izquierda, az=+90 derecha (el azDeg que llega ya está mapeado en el caller)
        float p = std::max(-1.0f, std::min(1.0f, azDeg / 90.0f));
        float theta = (p + 1.0f) * 0.25f * 3.14159265358979323846f; // (p+1)*pi/4
        float wL = std::cos(theta);
        float wR = std::sin(theta);
        // Mantener Top del VBAP pero sin valores negativos
        float top = g(1);
        if (top < 0.0f) top = 0.0f;
        g(0) = wL;   // Left
        g(1) = top;  // Top
        g(2) = wR;   // Right
        // Normalizar energía (RMS)
        float norm = std::sqrt(g.squaredNorm());
        if (norm > 1e-9f) g /= norm; else g.setZero();
        return g;
    }

    // compute per-output delay in fractional samples from source position (meters)
    // srcPosMeters: vector (x,y,z) in meters
    // altPosRows: same matrix used in setSpeakerPositions (rows = speakers)
    std::array<float,3> computeDelaysSamples(const Eigen::Vector3f& srcPosMeters, const Eigen::Matrix<float,3,3>& altPosRows) const
    {
        std::array<float,3> delays{0.0f,0.0f,0.0f};
        if (sampleRate <= 0.0 || c <= 0.0f) return delays;

        for (int i=0;i<3;++i)
        {
            Eigen::Vector3f sp = altPosRows.row(i).transpose();
            float d = (sp - srcPosMeters).norm(); // meters
            float dsamps = d * static_cast<float>(sampleRate) / c;
            delays[i] = dsamps;
        }
        return delays;
    }

    // processBlock: entrada mono (inBuf) puede tener N input channels summed, salida 3 channels interleaved in outBuf (AudioBuffer layout assumed handled by caller).
    // in: pointer to input samples (numSamples long). outPtrs: array of 3 pointers to output channels writeable.
    // altPosRows: speaker positions (rows)
    // srcPosMeters: position of the source in meters (x,y,z). If user provides az/el only, convert externally.
    // This function writes into outPtrs (adds to existing content).
    void processBlockFractionalDelay(const float* in, int numSamples,
                                     float* out0, float* out1, float* out2,
                                     const Eigen::Matrix<float,3,3>& altPosRows,
                                     const Eigen::Vector3f& srcPosMeters,
                                     float azDeg, float elDeg, float masterGain)
    {
        if (!in || numSamples <= 0 || maxDelaySamples <= 0) return;
        if (!out0 || !out1 || !out2) return;

        const int M = maxDelaySamples;
        // seguridad: delayBuf debe estar inicializado y tener tamaño M por canal
        for (int ch = 0; ch < 3; ++ch)
        {
            if ((int)delayBuf[ch].size() != M)
            {
                // protegerse: redimensionar si algo estuvo mal
                delayBuf[ch].assign(M, 0.0f);
                writeIndex[ch] = 0;
            }
            // asegurar que writeIndex está en rango
            if (writeIndex[ch] < 0 || writeIndex[ch] >= M) writeIndex[ch] %= M;
            if (writeIndex[ch] < 0) writeIndex[ch] += M;
        }

        Eigen::Vector3f gains = computeGainsDeg(azDeg, elDeg) * masterGain;
        auto delays = computeDelaysSamples(srcPosMeters, altPosRows); // asumimos valores en muestras (float)

        // validar delays: si alguno está fuera de rango, recortar o aumentar M
        for (int ch = 0; ch < 3; ++ch)
        {
            if (!std::isfinite(delays[ch])) delays[ch] = 0.0f;
            // si el delay es negativo, se interpreta como "adelanto" no válido: capear a 0
            if (delays[ch] < 0.0f) delays[ch] = 0.0f;
            // si el delay excede M-1, advertir y capear (o mejor: aumentar maxDelaySamples en init)
            if (delays[ch] > static_cast<float>(M - 1))
                delays[ch] = static_cast<float>(M - 1);
        }

        for (int n = 0; n < numSamples; ++n)
        {
            float s = in[n];

            // push s into each delay buffer at current write index
            for (int ch = 0; ch < 3; ++ch)
                delayBuf[ch][ writeIndex[ch] ] = s;

            // read and mix for each channel
            for (int ch = 0; ch < 3; ++ch)
            {
                float delay = delays[ch];
                float readPos = static_cast<float>(writeIndex[ch]) - delay;

                // normalize readPos a rango [0, M) de manera robusta
                // uso fmod con cuidado para valores positivos; fmod puede devolver negativo
                if (readPos < 0.0f || readPos >= static_cast<float>(M))
                {
                    float r = std::fmod(readPos, static_cast<float>(M));
                    if (r < 0.0f) r += static_cast<float>(M);
                    readPos = r;
                }

                // integer part and next sample (with modulo normalization)
                int idx1 = static_cast<int>(std::floor(readPos)); // puede estar en [0, M-1]
                int idx2 = idx1 + 1;

                // normalizar ambos índices con modulo positivo
                idx1 %= M; if (idx1 < 0) idx1 += M;
                idx2 %= M; if (idx2 < 0) idx2 += M;

                float frac = readPos - std::floor(readPos); // fracción en [0,1)

                // seguridad extra: clamp índices por si acaso
                if (idx1 < 0) idx1 = 0;
                else if (idx1 >= M) idx1 = M - 1;
                if (idx2 < 0) idx2 = 0;
                else if (idx2 >= M) idx2 = M - 1;

                float y = delayBuf[ch][idx1] * (1.0f - frac) + delayBuf[ch][idx2] * frac;
                float g = gains(ch);

                if (ch == 0) out0[n] += y * g;
                else if (ch == 1) out1[n] += y * g;
                else out2[n] += y * g;
            }

            // increment write indices (wrap)
            for (int ch = 0; ch < 3; ++ch)
            {
                ++writeIndex[ch];
                if (writeIndex[ch] >= M) writeIndex[ch] = 0;
            }
        }
    }

private:
    static inline float degToRad(float d) { return d * (3.14159265358979323846f / 180.0f); }

    static Eigen::Vector3f directionVectorDeg(float azDeg, float elDeg)
    {
        float az = degToRad(azDeg);
        float el = degToRad(elDeg);
        Eigen::Vector3f v;

        // Eje X = izq/der, Y = frente, Z = arriba
        // Convención: az=0 frente (+Y), +90 izquierda (-X), -90 derecha (+X)
        v(0) = -cos(el) * sin(az); // X (derecha positiva cuando az = -90)
        v(1) =  cos(el) * cos(az); // Y (frente)
        v(2) =  sin(el);           // Z (arriba)

        return v.normalized();
    }

    // speaker vectors (columns)
    Eigen::Matrix<float,3,3> speakerVecs = Eigen::Matrix<float,3,3>::Zero();
    Eigen::Matrix<float,3,3> A = Eigen::Matrix<float,3,3>::Zero();
    Eigen::Matrix<float,3,3> Ginv = Eigen::Matrix<float,3,3>::Zero();
    bool inverseValid = false;

    // fractional delay buffers (per output)
    std::vector<float> delayBuf[3];
    int writeIndex[3] = {0,0,0};
    int maxDelaySamples = 0;
    int maxBlock = 512;
    double sampleRate = 44100.0;
    float c = 343.0f; // speed of sound
};

