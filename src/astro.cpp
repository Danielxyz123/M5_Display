#include "app.h"
#include <math.h>

// =============================================================================
// Sonne und Mond für den Standort (ORT_LAT/ORT_LON), auf dem Gerät berechnet
//   Sonne: Sonnengleichung (Abweichung zu DWD/Open-Meteo <= 1 min)
//   Mond:  Beleuchtung nach Meeus Kap. 48, Neu-/Vollmond nach Meeus Kap. 49 (Hauptterme, ~1 min)
// Gleiches Verfahren wie astro.js in der VIS.
// =============================================================================
static const double RAD = M_PI / 180.0;
static inline double sn(double g) { return sin(g * RAD); }
static inline double cs(double g) { return cos(g * RAD); }

// Tage seit 1970-01-01 für ein Kalenderdatum (proleptisch gregorianisch)
static long tageSeit1970(int y, int m, int d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

// Sonnenzeit (UTC-Epoche) am lokalen Kalendertag; hoehe -0.833 = Auf-/Untergang, -6 = bürgerl. Dämmerung
static time_t sonnenZeit(const struct tm& tag, double hoehe, bool auf) {
  const long n = tageSeit1970(tag.tm_year + 1900, tag.tm_mon + 1, tag.tm_mday) - 10957;   // Tage seit 2000-01-01
  const double Js = n - ORT_LON / 360.0;
  const double M = fmod(357.5291 + 0.98560028 * Js, 360.0);
  const double C = 1.9148 * sn(M) + 0.02 * sn(2 * M) + 0.0003 * sn(3 * M);
  const double lam = fmod(M + C + 180 + 102.9372, 360.0);
  const double Jt = Js + 0.0053 * sn(M) - 0.0069 * sn(2 * lam);
  const double dec = asin(sn(lam) * sn(23.4397));
  const double cw = (sn(hoehe) - sn(ORT_LAT) * sin(dec)) / (cs(ORT_LAT) * cos(dec));
  if (cw < -1 || cw > 1) return 0;
  const double w = acos(cw) / RAD;
  const double J = auf ? Jt - w / 360.0 : Jt + w / 360.0;    // Tage seit 2000-01-01 12:00 UTC
  return (time_t)llround(946728000.0 + J * 86400.0);
}

SonneTag sonneBerechnen(time_t jetzt) {
  struct tm t; localtime_r(&jetzt, &t);
  SonneTag s;
  s.auf = sonnenZeit(t, -0.833, true);
  s.unter = sonnenZeit(t, -0.833, false);
  s.daemmUnter = sonnenZeit(t, -6, false);
  s.laengeMin = (s.auf && s.unter) ? (int)lround((s.unter - s.auf) / 60.0) : -1;
  return s;
}

// ---- Mond ----
static double jdVon(time_t t) { return t / 86400.0 + 2440587.5; }
static time_t epocheVon(double jd) { return (time_t)llround((jd - 2440587.5) * 86400.0); }

// Neumond (art 0) bzw. Vollmond (art 0.5) der Lunation k
static time_t mondPhaseK(long k, double art) {
  const double kk = k + art, T = kk / 1236.85;
  double J = 2451550.09766 + 29.530588861 * kk + 0.00015437 * T * T;
  const double E = 1 - 0.002516 * T;
  const double M = 2.5534 + 29.1053567 * kk, Mm = 201.5643 + 385.81693528 * kk;
  const double F = 160.7108 + 390.67050284 * kk, O = 124.7746 - 1.56375588 * kk;
  const bool voll = art > 0.25;
  J += (voll ? -0.40614 : -0.40720) * sn(Mm) + (voll ? 0.17302 : 0.17241) * E * sn(M) + (voll ? 0.01614 : 0.01608) * sn(2 * Mm)
     + (voll ? 0.01043 : 0.01039) * sn(2 * F) + (voll ? 0.00734 : 0.00739) * E * sn(Mm - M) + (voll ? -0.00515 : -0.00514) * E * sn(Mm + M)
     + (voll ? 0.00209 : 0.00208) * E * E * sn(2 * M) - 0.00111 * sn(Mm - 2 * F) - 0.00057 * sn(Mm + 2 * F) + 0.00056 * E * sn(2 * Mm + M)
     - 0.00042 * sn(3 * Mm) + 0.00042 * E * sn(M + 2 * F) + 0.00038 * E * sn(M - 2 * F) - 0.00024 * E * sn(2 * Mm - M) - 0.00017 * sn(O);
  return epocheVon(J);
}
static long kSchaetzung(time_t t) { return (long)floor((jdVon(t) - 2451550.1) / 29.530588861); }

time_t naechsterMond(time_t ab, bool voll) {
  for (long k = kSchaetzung(ab) - 1; k < kSchaetzung(ab) + 3; k++) {
    const time_t p = mondPhaseK(k, voll ? 0.5 : 0.0);
    if (p > ab) return p;
  }
  return 0;
}

MondInfo mondBerechnen(time_t jetzt) {
  const double T = (jdVon(jetzt) - 2451545.0) / 36525.0;
  const double D = fmod(297.8501921 + 445267.1114034 * T, 360.0);
  const double M = fmod(357.5291092 + 35999.0502909 * T, 360.0);
  const double Mm = fmod(134.9633964 + 477198.8675055 * T, 360.0);
  const double i = 180 - D - 6.289 * sn(Mm) + 2.100 * sn(M) - 1.274 * sn(2 * D - Mm) - 0.658 * sn(2 * D) - 0.214 * sn(2 * Mm) - 0.110 * sn(D);
  MondInfo m;
  m.beleuchtung = (float)((1 + cs(i)) / 2);
  m.zunehmend = fmod(fmod(D, 360.0) + 360.0, 360.0) < 180.0;
  const float b = m.beleuchtung;
  if (b < 0.03f)                    m.name = "Neumond";
  else if (b > 0.97f)               m.name = "Vollmond";
  else if (b >= 0.45f && b <= 0.55f) m.name = m.zunehmend ? "zunehmender Halbmond" : "abnehmender Halbmond";
  else if (b < 0.45f)               m.name = m.zunehmend ? "zunehmende Sichel" : "abnehmende Sichel";
  else                              m.name = m.zunehmend ? "zunehmender Mond" : "abnehmender Mond";
  return m;
}
