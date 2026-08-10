//
//  VBAPProcessor.h
//  Librería matemática header-only para Vector Base Amplitude Panning (VBAP)
//  con 3 altavoces. Replica el comportamiento del MATLAB de referencia:
//    1. setSpeakerPositions: NO normaliza las posiciones (A = altPos')
//    2. computeGainsXYZ: vecDir = src/|src|, g = A\vecDir, g /= |g|
//    3. Ganancias negativas clampeadas a 0 (fuera del triángulo de cobertura)
//    4. computeDelaysSamples: distancia/velocidad-del-sonido en muestras
//  El procesado por bloque (ring buffers, mezcla a salidas) lo hace el caller;
//  esta clase es puramente estado matemático: A, Ginv, sampleRate, c.
//

#pragma once
#include <Eigen/Dense>
#include <array>
#include <cmath>
#include <algorithm>

class VBAPProcessor
{
public:
    // Cota del número de condición de A. Por encima de esto la inversa
    // se considera numéricamente inestable.
    static constexpr float kMaxConditionNumber = 1.0e12f;

    VBAPProcessor() = default;

    // -------------------------------------------------------------------------
    //  setSpeakerPositions
    //  altPosRows: 3x3 — filas = altavoces, columnas = (x, y, z)
    //  MATLAB hace:  A = altPos'   (columnas = vectores de altavoz, SIN normalizar)
    //  Aquí hacemos lo mismo: A = altPosRows.transpose()
    // -------------------------------------------------------------------------
    void setSpeakerPositions(const Eigen::Matrix<float, 3, 3>& altPosRows)
    {
        // Columnas de A = posiciones de altavoces (raw, sin normalizar — igual que MATLAB)
        A = altPosRows.transpose();   // 3x3: cada columna es el vector de un altavoz

        float det = A.determinant();
        if (std::isnan(det) || std::abs(det) < 1e-9f)
        {
            inverseValid = false;
            return;
        }

        Eigen::Matrix<float, 3, 3> invA = A.inverse();
        float condA = A.norm() * invA.norm();
        if (std::isnan(condA) || condA > kMaxConditionNumber)
        {
            inverseValid = false;
        }
        else
        {
            Ginv = invA;
            inverseValid = true;
        }
    }

    // -------------------------------------------------------------------------
    //  setSampleRate  —  llámalo en prepareToPlay
    // -------------------------------------------------------------------------
    void setSampleRate(double fs, float speedOfSound_m_s = 343.0f) noexcept
    {
        sampleRate = (fs > 0.0) ? fs : 44100.0;
        c = (speedOfSound_m_s > 0.0f) ? speedOfSound_m_s : 343.0f;
    }

    // -------------------------------------------------------------------------
    //  computeGainsXYZ  —  replica exacta del MATLAB:
    //    vecDir = srcPos / norm(srcPos)
    //    g      = A \ vecDir          (A*g = vecDir, least-squares)
    //    g      = g / norm(g)
    //  Devuelve {0,0,0} si la matriz no es invertible.
    // -------------------------------------------------------------------------
    Eigen::Vector3f computeGainsXYZ(const Eigen::Vector3f& srcPosMeters) const
    {
        // Dirección unitaria hacia la fuente
        Eigen::Vector3f vecDir;
        float n = srcPosMeters.norm();
        if (n < 1e-9f)
            vecDir = Eigen::Vector3f(0.0f, 0.0f, 1.0f);   // fallback: arriba
        else
            vecDir = srcPosMeters / n;

        Eigen::Vector3f g;
        if (inverseValid)
            g = Ginv * vecDir;   // equivalente a A \ vecDir de MATLAB
        else
        {
            g.setZero();
            return g;
        }

        // CLAMP: ganancias negativas -> 0
        // Una ganancia negativa significa que la fuente esta fuera del area de
        // cobertura de ese altavoz. Reproducirla invertida (fase negativa) causa
        // confusion espacial: el oido percibe "izquierda" aunque la fuente este
        // a la derecha. Al clampear a 0 solo suenan los altavoces activos.
        for (int i = 0; i < 3; ++i)
            if (g(i) < 0.0f) g(i) = 0.0f;

        // Normalizar energia con las ganancias ya clampeadas
        float ng = g.norm();
        if (ng > 1e-9f)
            g /= ng;
        else
            g.setZero();

        return g;
    }

    // -------------------------------------------------------------------------
    //  computeDelaysSamples  —  igual que MATLAB:
    //    diff  = altPos(i,:) - srcPos
    //    dist  = norm(diff)
    //    delay = round(dist * fs / c)   (aquí devolvemos float para interpolación)
    // -------------------------------------------------------------------------
    std::array<float, 3> computeDelaysSamples(const Eigen::Vector3f& srcPosMeters,
        const Eigen::Matrix<float, 3, 3>& altPosRows) const
    {
        std::array<float, 3> delays{ 0.0f, 0.0f, 0.0f };
        if (sampleRate <= 0.0 || c <= 0.0f) return delays;

        for (int i = 0; i < 3; ++i)
        {
            Eigen::Vector3f sp = altPosRows.row(i).transpose();
            float d = (sp - srcPosMeters).norm();                          // metros
            delays[i] = d * static_cast<float>(sampleRate) / c;             // muestras (float)
        }
        return delays;
    }

    // Debug getters
    const Eigen::Matrix<float, 3, 3>& getA()    const noexcept { return A; }
    const Eigen::Matrix<float, 3, 3>& getGinv() const noexcept { return Ginv; }
    bool isInverseValid()                      const noexcept { return inverseValid; }

private:
    // Matriz A: columnas = vectores de los altavoces (sin normalizar, igual MATLAB)
    Eigen::Matrix<float, 3, 3> A = Eigen::Matrix<float, 3, 3>::Zero();
    Eigen::Matrix<float, 3, 3> Ginv = Eigen::Matrix<float, 3, 3>::Zero();
    bool   inverseValid = false;
    double sampleRate   = 44100.0;
    float  c            = 343.0f;
};
