/*
 * Medical Image Registration ToolKit (MIRTK)
 *
 * Copyright 2016 Imperial College London
 * Copyright 2016 Andreas Schuh
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mirtk/ImageEdgeDistance.h"

#include "mirtk/Math.h"
#include "mirtk/Parallel.h"
#include "mirtk/Profiling.h"
#include "mirtk/DataStatistics.h"
#include "mirtk/MeshSmoothing.h"
#include "mirtk/MedianMeshFilter.h"
#include "mirtk/FastCubicBSplineInterpolateImageFunction.h"

#include "mirtk/PointSetIO.h"
#include "mirtk/PointSetUtils.h"

#include "vtkPointData.h"


namespace mirtk {


// Register energy term with object factory during static initialization
mirtkAutoRegisterEnergyTermMacro(ImageEdgeDistance);


// =============================================================================
// Auxiliary functions
// =============================================================================

namespace ImageEdgeDistanceUtils {


// Type of discrete intensity image
typedef GenericImage<double> DiscreteImage;

// Type of local intensity statistics image
typedef ImageEdgeDistance::LocalStatsImage LocalStatsImage;

// Type of interpolated image
typedef GenericFastCubicBSplineInterpolateImageFunction<DiscreteImage> ContinuousImage;

// -----------------------------------------------------------------------------
/// Compute global intensity statistics
struct ComputeGlobalStatistics : public VoxelReduction
{
private:

  int    _Num;
  double _Sum;
  double _Sum2;

public:

  ComputeGlobalStatistics() : _Num(0), _Sum(0.), _Sum2(0.) {}

  void split(const ComputeGlobalStatistics &)
  {
    _Num = 0;
    _Sum = _Sum2 = 0.;
  }

  void join(const ComputeGlobalStatistics &other)
  {
    _Num  += other._Num;
    _Sum  += other._Sum;
    _Sum2 += other._Sum2;
  }

  template <class TIn, class TMask>
  void operator()(int, int, int, int, const TIn *in, const TMask *mask)
  {
    if (*mask != 0) {
      _Num  += 1;
      _Sum  += (*in);
      _Sum2 += (*in) * (*in);
    }
  }

  double Mean() const
  {
    return (_Num == 0 ? 0. : _Sum / _Num);
  }

  double Variance() const
  {
    const double mean = Mean();
    return (_Num == 0 ? 0. : (_Sum2 / _Num) - mean * mean);
  }
};

// -----------------------------------------------------------------------------
/// Compute local intensity statistics
class ComputeLocalStatistics : public VoxelFunction
{
  int    _Radius;
  int    _MinNumberOfSamples;
  double _GlobalMean;
  double _GlobalVariance;

public:

  ComputeLocalStatistics(const ImageAttributes &attr, int width, double global_mean = 0., double global_variance = 0.)
  :
    _Radius(width / 2), _GlobalMean(global_mean), _GlobalVariance(global_variance)
  {
    int max_nsamples = 1;
    if (attr._x > 1) max_nsamples *= width;
    if (attr._y > 1) max_nsamples *= width;
    if (attr._z > 1) max_nsamples *= width;
    _MinNumberOfSamples = 5 * max_nsamples / 100;
  }

  template <class TIn, class TMask, class TOut>
  void operator ()(int ci, int cj, int ck, int cl, const TIn *in, const TMask *mask, TOut *mean, TOut *var)
  {
    int    num = 0;
    double sum = 0., sum2 = 0., v;

    const int nx = _Domain->_x;
    const int ny = _Domain->_y;
    const int nz = _Domain->_z;

    const int i1 = max(0, ci - _Radius), i2 = min(ci + _Radius, nx - 1);
    const int j1 = max(0, cj - _Radius), j2 = min(cj + _Radius, ny - 1);
    const int k1 = max(0, ck - _Radius), k2 = min(ck + _Radius, nz - 1);

    const int xstride = 1;
    const int ystride =  nx - (i2 - i1 + 1);
    const int zstride = (ny - (j2 - j1 + 1)) * nx;
    const int offset  = _Domain->LatticeToIndex(i1, j1, k1) - _Domain->LatticeToIndex(ci, cj, ck);

    in -= offset, mask -= offset;
    for (int k = k1; k <= k2; ++k, in += zstride, mask += zstride)
    for (int j = j1; j <= j2; ++j, in += ystride, mask += ystride)
    for (int i = i1; i <= i2; ++i, in += xstride, mask += xstride) {
      if (*mask != 0) {
        v = static_cast<double>(*in);
        sum += v, sum2 += v * v, ++num;
      }
    }
    if (num >= _MinNumberOfSamples) {
      sum /= num, sum2 /= num;
      *mean = sum;
      *var  = sum2 - sum * sum;
    } else {
      *mean = _GlobalMean;
      *var  = _GlobalVariance;
    }
  }
};

// -----------------------------------------------------------------------------
/// Compute distance to closest image edge
struct ComputeDistances
{
  vtkPoints    *_Points;
  vtkDataArray *_Status;
  vtkDataArray *_Normals;

  vtkDataArray *_ImageGradient;
  vtkDataArray *_Distances;
  vtkDataArray *_Magnitude;

  const ContinuousImage *_Image;
  const LocalStatsImage *_LocalWhiteMatterMean;
  const LocalStatsImage *_LocalWhiteMatterVariance;
  const LocalStatsImage *_LocalGreyMatterMean;
  const LocalStatsImage *_LocalGreyMatterVariance;

  double _Padding;
  double _MinIntensity;
  double _MaxIntensity;
  double _MinGradient;
  double _MaxDistance;
  double _StepLength;
  double _GlobalWhiteMatterMean;
  double _GlobalWhiteMatterSigma;
  double _GlobalWhiteMatterVariance;
  double _GlobalGreyMatterMean;
  double _GlobalGreyMatterVariance;

  enum ImageEdgeDistance::EdgeType _EdgeType;
  typedef Vector3D<int> Voxel;

  // ---------------------------------------------------------------------------
  inline Point RayPoint(const Point &p, const Vector3 &dp, int i, int k) const
  {
    return p + static_cast<double>(i - (k - 1) / 2) * dp;
  }

  // ---------------------------------------------------------------------------
  inline void SampleIntensity(Array<double> &f, Point p, const Vector3 &dp) const
  {
    p -= static_cast<double>((f.size() - 1) / 2) * dp;
    for (size_t i = 0; i < f.size(); ++i, p += dp) {
      f[i] = _Image->Evaluate(p.x, p.y, p.z);
    }
  }

  // ---------------------------------------------------------------------------
  inline double SampleIntensity(Point p, const Vector3 &dp, int i, int k) const
  {
    p += static_cast<double>(i - (k - 1) / 2) * dp;
    return _Image->Evaluate(p.x, p.y, p.z);
  }

  // ---------------------------------------------------------------------------
  inline void SampleGradient(Array<double> &g, Point p, const Vector3 &dp) const
  {
    Matrix jac(1, 3);
    Vector3 n = dp;
    n.Normalize();
    int vi, vj, vk;
    p -= static_cast<double>((g.size() - 1) / 2) * dp;
    for (size_t i = 0; i < g.size(); ++i, p += dp) {
      vi = iround(p.x);
      vj = iround(p.y);
      vk = iround(p.z);
      if (_Image->Input()->IsInside(vi, vj, vk) && _Image->Input()->IsForeground(vi, vj, vk)) {
        _Image->Jacobian3D(jac, p.x, p.y, p.z);
        g[i] = n.x * jac(0, 0) + n.y * jac(0, 1) + n.z * jac(0, 2);
      } else {
        g[i] = NaN;
      }
    }
  }

  // ---------------------------------------------------------------------------
  inline int ClosestMinimum(const Array<double> &g) const
  {
    const int k  = static_cast<int>(g.size());
    const int i0 = (k - 1) / 2;

    auto i1 = i0;
    while (i1 < k - 1 && IsNaN(g[i1]))    ++i1;
    while (i1 < k - 1 && g[i1] > g[i1+1]) ++i1;

    auto i2 = i0;
    while (i2 > 0 && IsNaN(g[i2]))    --i2;
    while (i2 > 0 && g[i2] > g[i2-1]) --i2;

    return (g[i2] > g[i1] ? i2 : i1);
  }

  // ---------------------------------------------------------------------------
  inline int ClosestMaximum(const Array<double> &g) const
  {
    const int k  = static_cast<int>(g.size());
    const int i0 = (k - 1) / 2;

    auto i1 = i0;
    while (i1 < k - 1 && IsNaN(g[i1]))    ++i1;
    while (i1 < k - 1 && g[i1] < g[i1+1]) ++i1;

    auto i2 = i0;
    while (i2 > 0 && IsNaN(g[i2]))    --i2;
    while (i2 > 0 && g[i2] < g[i2-1]) --i2;

    return (g[i2] > g[i1] ? i2 : i1);
  }

  // ---------------------------------------------------------------------------
  inline int StrongestMinimum(const Array<double> &g) const
  {
    const int k  = static_cast<int>(g.size());
    const int i0 = (k - 1) / 2;

    auto i1 = i0;
    while (i1 < k - 1 && IsNaN(g[i1])) ++i1;
    for (auto i = i1 + 1; i < k; ++i) {
      if (g[i] < g[i1]) i1 = i;
    }

    auto i2 = i0;
    while (i2 > 0 && IsNaN(g[i2])) --i2;
    for (auto i = i2 - 1; i >= 0; --i) {
      if (g[i] < g[i2]) i2 = i;
    }

    return (g[i2] < g[i1] ? i2 : i1);
  }

  // ---------------------------------------------------------------------------
  inline int StrongestMaximum(const Array<double> &g) const
  {
    const int k  = static_cast<int>(g.size());
    const int i0 = (k - 1) / 2;

    auto i1 = i0;
    while (i1 < k - 1 && IsNaN(g[i1])) ++i1;
    for (auto i = i1 + 1; i < k; ++i) {
      if (g[i] > g[i1]) i1 = i;
    }

    auto i2 = i0;
    while (i2 > 0 && IsNaN(g[i2])) --i2;
    for (auto i = i2 - 1; i >= 0; --i) {
      if (g[i] < g[i2]) i2 = i;
    }

    return (g[i2] > g[i1] ? i2 : i1);
  }

  // ---------------------------------------------------------------------------
  inline int NextMinimumValue(const Array<double> &v, int i0, int di) const
  {
    int i = i0 + di;
    const int k = static_cast<int>(v.size());
    while (0 < i && i < k-1 && (IsNaN(v[i]) || v[i] < v[i + di])) i += di;
    if (i == 0 || i == k-1) return i0;
    while (0 < i && i < k-1 && v[i] > v[i + di]) i += di;
    return i;
  }

  // ---------------------------------------------------------------------------
  inline int NextMaximumValue(const Array<double> &v, int i0, int di) const
  {
    int i = i0 + di;
    const int k = static_cast<int>(v.size());
    while (0 < i && i < k-1 && (IsNaN(v[i]) || v[i] > v[i + di])) i += di;
    if (i == 0 || i == k-1) return i0;
    while (0 < i && i < k-1 && v[i] < v[i + di]) i += di;
    return i;
  }

  // ---------------------------------------------------------------------------
  inline double NextPositiveValue(const Array<double> &v, int i0, int di, double eps = 0.) const
  {
    int i = i0 + di;
    const int k = static_cast<int>(v.size());
    while (0 < i && i < k-1 && (v[i] <= eps || v[i] < v[i + di])) i += di;
    return i;
  }

  // ---------------------------------------------------------------------------
  /// Determine distance in mm to next positive peak along outward normal direction
  inline double FrontfaceDistanceToMaxPositiveGradient(const Array<double> &g, int i0) const
  {
    int i = i0;
    const int k = static_cast<int>(g.size());
    while (i < k && (g[i] < _MinGradient || g[i] < g[i+1])) ++i;
    return (i - i0) * _StepLength;
  }

  // ---------------------------------------------------------------------------
  /// Find image edge of WM/cGM boundary in T2-weighted MRI of neonatal brain
  ///
  /// The initial surface for the deformation process is the white surface
  /// obtained by deforming a sphere/convex hull towards the white matter
  /// tissue segmentation mask. The surface thus is close to the target boundary
  /// and should only be refined using this force.
  inline int NeonatalWhiteSurface(const Point &p, const Vector3 &dp, const Array<double> &f, const Array<double> &g) const
  {
    int i, j;

    const int    k  = static_cast<int>(g.size());
    const int    i0 = (k - 1) / 2;
    const double g1 = -_MinGradient;
    const double g2 = +_MinGradient;

    i = i0;
    while (1 < i && (g[i] >= g1 || g[i] >= g[i-1])) --i;
    j = i + 1;
    while (j < k - 2 && (g[j] - g[j-1]) * (g[j] - g[j+1]) <= 0.) ++j;
    int i2 = (g[i] < g1 && g[j] > 0. ? i : -1);

    i = i0;
    while (i < k - 2 && ((g1 <= g[i] && g[i] <= g2) || g[i] >= g[i+1])) ++i;
    j = i + 1;
    while (j < k - 2 && (g[j] - g[j-1]) * (g[j] - g[j+1]) <= 0.) ++j;
    int i1 = (g[i] < g1 && g[j] > 0. ? i : -1);

    if (i1 != -1 && i2 != -1) {
      int    iw, ig;
      double score1 = 0., score2 = 0.;
      double wm_mean, wm_var, gm_mean, gm_var;

      const Point vox = RayPoint(p, dp, i0, k);
      const int vi = iround(vox._x);
      const int vj = iround(vox._y);
      const int vk = iround(vox._z);

      iw = i1;
      while (0 < iw && f[iw-1] >= f[iw]) --iw;
      if (f[iw] > _MaxIntensity) {
        i1 = i0;
      } else {
        ig = i1;
        while (ig < k-1 && f[ig+1] <= f[ig]) ++ig;
        if (f[ig] < _MinIntensity || f[ig] < _Padding) {
          i1 = i0;
        } else {
          wm_mean = _GlobalWhiteMatterMean;
          wm_var  = _GlobalWhiteMatterVariance;
          gm_mean = _GlobalGreyMatterMean;
          gm_var  = _GlobalGreyMatterVariance;
          if (_LocalWhiteMatterMean) {
            wm_mean = _LocalWhiteMatterMean->Get(vi, vj, vk);
          }
          if (_LocalWhiteMatterVariance) {
            wm_var = _LocalWhiteMatterVariance->Get(vi, vj, vk);
          }
          if (_LocalGreyMatterMean) {
            gm_mean = _LocalGreyMatterMean->Get(vi, vj, vk);
          }
          if (_LocalGreyMatterVariance) {
            gm_var = _LocalGreyMatterVariance->Get(vi, vj, vk);
          }
          if (IsNaN(wm_mean) || IsNaN(wm_var) || wm_var == 0.) {
            score1 = abs(g[i1]);
          } else {
            score1 = exp(-.5 * pow(f[iw] - wm_mean, 2) / wm_var);
          }
          if (f[ig] > gm_mean && !IsNaN(gm_var) && gm_var > 0.) {
            score1 *= exp(-.5 * pow(f[ig] - gm_mean, 2) / gm_var);
          }
        }
      }

      iw = i2;
      while (0 < iw && f[iw-1] >= f[iw]) --iw;
      if (f[iw] > _MaxIntensity) {
        i2 = i0;
      } else {
        ig = i2;
        while (ig < k-1 && f[ig+1] <= f[ig]) ++ig;
        if (f[ig] < _MinIntensity || f[ig] < _Padding) {
          i2 = i0;
        } else {
          wm_mean = _GlobalWhiteMatterMean;
          wm_var  = _GlobalWhiteMatterVariance;
          gm_mean = _GlobalGreyMatterMean;
          gm_var  = _GlobalGreyMatterVariance;
          if (_LocalWhiteMatterMean) {
            wm_mean = _LocalWhiteMatterMean->Get(vi, vj, vk);
          }
          if (_LocalWhiteMatterVariance) {
            wm_var = _LocalWhiteMatterVariance->Get(vi, vj, vk);
          }
          if (_LocalGreyMatterMean) {
            gm_mean = _LocalGreyMatterMean->Get(vi, vj, vk);
          }
          if (_LocalGreyMatterVariance) {
            gm_var = _LocalGreyMatterVariance->Get(vi, vj, vk);
          }
          if (IsNaN(wm_mean) || IsNaN(wm_var) || wm_var == 0.) {
            score2 = abs(g[i2]);
          } else {
            score2 = exp(-.5 * pow(f[iw] - wm_mean, 2) / wm_var);
          }
          if (f[ig] > gm_mean && !IsNaN(gm_var) && gm_var > 0.) {
            score2 *= exp(-.5 * pow(f[ig] - gm_mean, 2) / gm_var);
          }
        }
      }

      return (score2 < score1 ? i1 : i2);
    } else if (i1 != -1) {
      return i1;
    } else if (i2 != -1) {
      return i2;
    } else {
      return i0;
    }
  }

  // ---------------------------------------------------------------------------
  /// Find image edge of cGM/CSF boundary in T2-weighted MRI of neonatal brain
  ///
  /// The initial surface for the deformation process is the white surface
  /// delineating the WM/cGM boundary. The image foreground (mask) should
  /// exclude the interior of this initial surface such that the pial surface
  /// may only deform outwards from this initial surface mesh.
  inline int NeonatalPialSurface(const Array<double> &g) const
  {
    const int k  = static_cast<int>(g.size());
    const int i0 = (k - 1) / 2;

    auto i = i0;
    while (i < k - 1 && IsNaN(g[i])) ++i;
    while (i < k - 1 && (g[i] <= _MinGradient || g[i] < g[i+1])) ++i;
    const auto i1 = (g[i] > 0. ? i : -1);

    i = i0;
    while (i > 0 && (g[i] <= _MinGradient || g[i] < g[i-1])) --i;
    const auto i2 = (g[i] > 0. ? i : -1);

    if (i1 != -1 && i2 != -1) {
      return (abs(i0 - i1) <= abs(i0 - i2) ? i1 : i2);
    } else if (i1 != -1) {
      return i1;
    } else if (i2 != -1) {
      return i2;
    } else {
      return i0;
    }
  }

  // ---------------------------------------------------------------------------
  void operator ()(const blocked_range<int> &ptIds) const
  {
    const int r = ifloor(_MaxDistance / _StepLength);
    const int k = 2 * r + 1;

    double  value;
    int     i, j, j1, j2;
    Point   p;
    Vector3 n;

    Array<double> g(k), f;
    if (_EdgeType == ImageEdgeDistance::NeonatalWhiteSurface) f.resize(k);

    for (int ptId = ptIds.begin(); ptId != ptIds.end(); ++ptId) {
      if (_Status && _Status->GetComponent(ptId, 0) == 0.) {
        _Distances->SetComponent(ptId, 0, 0.);
        _Magnitude->SetComponent(ptId, 0, 0.);
        continue;
      }
      // Get point position and scaled normal
      _Points ->GetPoint(ptId, p);
      _Normals->GetTuple(ptId, n), n *= _StepLength;
      // Transform point/vector to image space
      _Image->WorldToImage(p);
      _Image->WorldToImage(n);
      // Sample image gradient along cast ray
      SampleGradient(g, p, n);
      // Find edge in normal direction
      switch (_EdgeType) {
        case ImageEdgeDistance::Extremum: {
          if      (g[r] < 0.) j = ClosestMinimum(g);
          else if (g[r] > 0.) j = ClosestMaximum(g);
          else                j = r;
        } break;
        case ImageEdgeDistance::ClosestMinimum: {
          j = ClosestMinimum(g);
        } break;
        case ImageEdgeDistance::ClosestMaximum: {
          j = ClosestMaximum(g);
        } break;
        case ImageEdgeDistance::ClosestExtremum: {
          j1 = ClosestMinimum(g);
          j2 = ClosestMaximum(g);
          j  = (abs(j1 - r) < abs(j2 - r) ? j1 : j2);
        } break;
        case ImageEdgeDistance::StrongestMinimum: {
          j = StrongestMinimum(g);
        } break;
        case ImageEdgeDistance::StrongestMaximum: {
          j = StrongestMaximum(g);
        } break;
        case ImageEdgeDistance::StrongestExtremum: {
          j1 = StrongestMinimum(g);
          j2 = StrongestMaximum(g);
          j  = (abs(g[j1]) > abs(g[j2]) ? j1 : j2);
        } break;
        case ImageEdgeDistance::NeonatalWhiteSurface: {
          SampleIntensity(f, p, n);
          j = NeonatalWhiteSurface(p, n, f, g);
        } break;
        case ImageEdgeDistance::NeonatalPialSurface: {
          j = NeonatalPialSurface(g);
        } break;
      }
      // When intensity thresholds set, use them to ignore irrelevant edges
      if (_EdgeType != ImageEdgeDistance::NeonatalWhiteSurface) {
        if (j != r && (!IsInf(_MinIntensity) || !IsInf(_MaxIntensity))) {
          value = SampleIntensity(p, n, j, k);
          if (value < _MinIntensity || value > _MaxIntensity) {
            j = r;
          }
        }
        if (j != r && !IsInf(_Padding)) {
          if (j < r) {
            i = r;
            for (i = r; i > 0; --i) {
              if (f[i] < _Padding) {
                i = 0;
                break;
              }
              if (g[j] * g[i] < 0.) break;
            }
            if (i == 0) j = r;
          } else if (j > r) {
            i = r;
            for (i = r; i < k; ++i) {
              if (f[i] < _Padding) {
                i = k;
                break;
              }
              if (g[j] * g[i] < 0.) break;
            }
            if (i == k) j = r;
          }
        }
      }
      // Set point distance to found edge and edge strength
      _Distances->SetComponent(ptId, 0, static_cast<double>(j - r) * _StepLength);
      _Magnitude->SetComponent(ptId, 0, IsNaN(g[j]) ? 0. : abs(g[j]));
    }
  }
};

// -----------------------------------------------------------------------------
/// Compute magnitude of image edge force
struct ComputeMagnitude
{
  vtkDataArray *_Status;
  vtkDataArray *_Distances;
  double        _DistanceScale;
  double        _MaxMagnitude;
  vtkDataArray *_Magnitude;

  void operator ()(const blocked_range<int> &ptIds) const
  {
    double d, d2, m1 = 1., m2;
    for (auto ptId = ptIds.begin(); ptId != ptIds.end(); ++ptId) {
      if (_Status && _Status->GetComponent(ptId, 0) == 0.) {
        _Magnitude->SetComponent(ptId, 0, 0.);
      } else {
        // Edge magnitude factor
        m1 = _Magnitude->GetComponent(ptId, 0);
        m1 = SShapedMembershipFunction(m1, 0., _MaxMagnitude);
        // Edge distance factor
        d  = _Distances->GetComponent(ptId, 0);
        d2 = _DistanceScale * d, d2 *= d2;
        m2 = d2 / (1. + d2);
        // Force magnitude
        _Magnitude->SetComponent(ptId, 0, m1 * copysign(m2, d));
      }
    }
  }
};

// -----------------------------------------------------------------------------
/// Compute force term penalty
struct ComputePenalty
{
  vtkDataArray *_Distances;
  double        _Sum;

  ComputePenalty() : _Sum(0.) {}

  ComputePenalty(const ComputePenalty &other, split)
  :
    _Distances(other._Distances), _Sum(0.)
  {}

  void join(const ComputePenalty &other)
  {
    _Sum += other._Sum;
  }

  void operator ()(const blocked_range<int> &ptIds)
  {
    for (auto ptId = ptIds.begin(); ptId != ptIds.end(); ++ptId) {
      _Sum += abs(_Distances->GetComponent(ptId, 0));
    }
  }
};

// -----------------------------------------------------------------------------
/// Compute gradient of force term, i.e., the negative force
struct ComputeGradient
{
  typedef ImageEdgeDistance::GradientType GradientType;

  vtkDataArray *_Normals;
  vtkDataArray *_Magnitude;
  GradientType *_Gradient;

  void operator ()(const blocked_range<int> &ptIds) const
  {
    double m, n[3];
    for (auto ptId = ptIds.begin(); ptId != ptIds.end(); ++ptId) {
      _Normals->GetTuple(ptId, n);
      m = _Magnitude->GetComponent(ptId, 0);
      _Gradient[ptId] = -m * GradientType(n);
    }
  }
};


} // namespace ImageEdgeDistanceUtils
using namespace ImageEdgeDistanceUtils;

// =============================================================================
// Enum <-> string conversion
// =============================================================================

// -----------------------------------------------------------------------------
template <>
bool FromString(const char *str, enum ImageEdgeDistance::EdgeType &value)
{
  const string lstr = ToLower(str);
  if (lstr == "extremum") {
    value = ImageEdgeDistance::Extremum;
  } else if (lstr == "closestminimum" || lstr == "closest minimum" ||
             lstr == "localminimum"   || lstr == "local minimum"   ||
             lstr == "minimum" || lstr == "min") {
    value = ImageEdgeDistance::ClosestMinimum;
  } else if (lstr == "closestmaximum" || lstr == "closest maximum" ||
             lstr == "localmaximum"   || lstr == "local maximum"   ||
             lstr == "maximum" || lstr == "max") {
    value = ImageEdgeDistance::ClosestMaximum;
  } else if (lstr == "closestextremum" || lstr == "closest extremum") {
    value = ImageEdgeDistance::ClosestExtremum;
  } else if (lstr == "strongestminimum" || lstr == "strongest minimum") {
    value = ImageEdgeDistance::StrongestMinimum;
  } else if (lstr == "strongestmaximum" || lstr == "strongest maximum") {
    value = ImageEdgeDistance::StrongestMaximum;
  } else if (lstr == "strongestextremum" || lstr == "strongest extremum") {
    value = ImageEdgeDistance::StrongestExtremum;
  } else if (lstr == "neonatal white surface" || lstr == "neonatal white" ||
             lstr == "neonatal t2-w wm/cgm"   || lstr == "neonatal t2-w cgm/wm") {
    value = ImageEdgeDistance::NeonatalWhiteSurface;
  } else if (lstr == "neonatal pial surface" || lstr == "neonatal pial" ||
             lstr == "neonatal t2-w cgm/csf" || lstr == "neonatal t2-w csf/cgm") {
    value = ImageEdgeDistance::NeonatalPialSurface;
  } else {
    return false;
  }
  return true;
}

// -----------------------------------------------------------------------------
template <>
string ToString(const enum ImageEdgeDistance::EdgeType &value, int w, char c, bool left)
{
  const char *str;
  switch (value) {
    case ImageEdgeDistance::Extremum:             { str = "Extremum"; } break;
    case ImageEdgeDistance::ClosestMinimum:       { str = "ClosestMinimum"; } break;
    case ImageEdgeDistance::ClosestMaximum:       { str = "ClosestMaximum"; } break;
    case ImageEdgeDistance::ClosestExtremum:      { str = "ClosestExtremum"; } break;
    case ImageEdgeDistance::StrongestMinimum:     { str = "StrongestMinimum"; } break;
    case ImageEdgeDistance::StrongestMaximum:     { str = "StrongestMaximum"; } break;
    case ImageEdgeDistance::StrongestExtremum:    { str = "StrongestExtremum"; } break;
    case ImageEdgeDistance::NeonatalWhiteSurface: { str = "Neonatal T2-w WM/cGM"; } break;
    case ImageEdgeDistance::NeonatalPialSurface:  { str = "Neonatal T2-w cGM/CSF"; } break;
  }
  return ToString(str, w, c, left);
}

// =============================================================================
// Construction/Destruction
// =============================================================================

// -----------------------------------------------------------------------------
void ImageEdgeDistance::CopyAttributes(const ImageEdgeDistance &other)
{
  _EdgeType           = other._EdgeType;
  _Padding            = other._Padding;
  _MinIntensity       = other._MinIntensity;
  _MaxIntensity       = other._MaxIntensity;
  _MinGradient        = other._MinGradient;
  _MaxDistance        = other._MaxDistance;
  _MedianFilterRadius = other._MedianFilterRadius;
  _DistanceSmoothing  = other._DistanceSmoothing;
  _MagnitudeSmoothing = other._MagnitudeSmoothing;
  _StepLength         = other._StepLength;

  _WhiteMatterMask           = other._WhiteMatterMask;
  _GreyMatterMask            = other._GreyMatterMask;
  _WhiteMatterWindowWidth    = other._WhiteMatterWindowWidth;
  _GreyMatterWindowWidth     = other._GreyMatterWindowWidth;
  _GlobalWhiteMatterMean     = other._GlobalWhiteMatterMean;
  _GlobalWhiteMatterVariance = other._GlobalWhiteMatterVariance;
  _GlobalGreyMatterMean      = other._GlobalGreyMatterMean;
  _GlobalGreyMatterVariance  = other._GlobalGreyMatterVariance;
  _LocalWhiteMatterMean      = other._LocalWhiteMatterMean;
  _LocalWhiteMatterVariance  = other._LocalWhiteMatterVariance;
  _LocalGreyMatterMean       = other._LocalGreyMatterMean;
  _LocalGreyMatterVariance   = other._LocalGreyMatterVariance;
}

// -----------------------------------------------------------------------------
ImageEdgeDistance::ImageEdgeDistance(const char *name, double weight)
:
  SurfaceForce(name, weight),
  _EdgeType(Extremum),
  _Padding(-inf),
  _MinIntensity(-inf),
  _MaxIntensity(+inf),
  _MinGradient(0.),
  _MaxDistance(0.),
  _MedianFilterRadius(0),
  _DistanceSmoothing(0),
  _MagnitudeSmoothing(2),
  _StepLength(1.),
  _WhiteMatterMask(nullptr),
  _GreyMatterMask(nullptr),
  _WhiteMatterWindowWidth(0),
  _GreyMatterWindowWidth(0),
  _GlobalWhiteMatterMean(NaN),
  _GlobalWhiteMatterVariance(NaN),
  _GlobalGreyMatterMean(NaN),
  _GlobalGreyMatterVariance(NaN)
{
  _ParameterPrefix.push_back("Image edge distance ");
  _ParameterPrefix.push_back("Intensity edge distance ");
  _ParameterPrefix.push_back("Edge distance ");
}

// -----------------------------------------------------------------------------
ImageEdgeDistance::ImageEdgeDistance(const ImageEdgeDistance &other)
:
  SurfaceForce(other)
{
  CopyAttributes(other);
}

// -----------------------------------------------------------------------------
ImageEdgeDistance &ImageEdgeDistance::operator =(const ImageEdgeDistance &other)
{
  if (this != &other) {
    SurfaceForce::operator =(other);
    CopyAttributes(other);
  }
  return *this;
}

// -----------------------------------------------------------------------------
ImageEdgeDistance::~ImageEdgeDistance()
{
}

// =============================================================================
// Configuration
// =============================================================================

// -----------------------------------------------------------------------------
bool ImageEdgeDistance::SetWithoutPrefix(const char *param, const char *value)
{
  if (strcmp(param, "Type") == 0 || strcmp(param, "Mode") == 0) {
    return FromString(value, _EdgeType);
  }
  if (strcmp(param, "Maximum") == 0 || strcmp(param, "Maximum distance") == 0) {
    return FromString(value, _MaxDistance);
  }
  if (strcmp(param, "Intensity threshold") == 0 || strcmp(param, "Padding") == 0) {
    return FromString(value, _Padding);
  }
  if (strcmp(param, "Lower intensity threshold") == 0  || strcmp(param, "Lower threshold") == 0 || strcmp(param, "Minimum intensity") == 0 || strcmp(param, "Intensity threshold") == 0) {
    return FromString(value, _MinIntensity);
  }
  if (strcmp(param, "Upper intensity threshold") == 0 || strcmp(param, "Upper intensity") == 0 || strcmp(param, "Maximum intensity") == 0) {
    return FromString(value, _MaxIntensity);
  }
  if (strcmp(param, "Minimum gradient") == 0 || strcmp(param, "Minimum gradient magnitude") == 0) {
    return FromString(value, _MinGradient);
  }
  if (strcmp(param, "Median filtering") == 0 || strcmp(param, "Median filter radius") == 0) {
    return FromString(value, _MedianFilterRadius);
  }
  if (strcmp(param, "Smoothing iterations")          == 0 ||
      strcmp(param, "Distance smoothing")            == 0 ||
      strcmp(param, "Distance smoothing iterations") == 0) {
    return FromString(value, _DistanceSmoothing);
  }
  if (strcmp(param, "Magnitude smoothing")            == 0 ||
      strcmp(param, "Magnitude smoothing iterations") == 0) {
    return FromString(value, _MagnitudeSmoothing);
  }
  if (strcmp(param, "Local white matter window width") == 0) {
    return FromString(value, _WhiteMatterWindowWidth);
  }
  if (strcmp(param, "Local white matter window radius") == 0) {
    int radius;
    if (!FromString(value, radius)) return false;
    _WhiteMatterWindowWidth = 2 * radius + 1;
    return true;
  }
  if (strcmp(param, "Local grey matter window width") == 0) {
    return FromString(value, _GreyMatterWindowWidth);
  }
  if (strcmp(param, "Local grey matter window radius") == 0) {
    int radius;
    if (!FromString(value, radius)) return false;
    _GreyMatterWindowWidth = 2 * radius + 1;
    return true;
  }
  if (strcmp(param, "Local window width") == 0) {
    int width;
    if (!FromString(value, width)) return false;
    _WhiteMatterWindowWidth = _GreyMatterWindowWidth = width;
    return false;
  }
  if (strcmp(param, "Local window radius") == 0) {
    int radius;
    if (!FromString(value, radius)) return false;
    _WhiteMatterWindowWidth = _GreyMatterWindowWidth = 2 * radius + 1;
    return true;
  }
  return SurfaceForce::SetWithoutPrefix(param, value);
}

// -----------------------------------------------------------------------------
ParameterList ImageEdgeDistance::Parameter() const
{
  ParameterList params = SurfaceForce::Parameter();
  InsertWithPrefix(params, "Type",                 _EdgeType);
  InsertWithPrefix(params, "Maximum",              _MaxDistance);
  InsertWithPrefix(params, "Intensity threshold",  _Padding);
  InsertWithPrefix(params, "Lower intensity",      _MinIntensity);
  InsertWithPrefix(params, "Upper intensity",      _MaxIntensity);
  InsertWithPrefix(params, "Minimum gradient magnitude", _MinGradient);
  InsertWithPrefix(params, "Median filter radius", _MedianFilterRadius);
  InsertWithPrefix(params, "Smoothing iterations", _DistanceSmoothing);
  InsertWithPrefix(params, "Magnitude smoothing",  _MagnitudeSmoothing);
  InsertWithPrefix(params, "Local white matter window width", _WhiteMatterWindowWidth);
  InsertWithPrefix(params, "Local grey matter window width", _GreyMatterWindowWidth);
  return params;
}

// =============================================================================
// Initialization
// =============================================================================

// -----------------------------------------------------------------------------
void ImageEdgeDistance::Initialize()
{
  // Initialize base class
  SurfaceForce::Initialize();
  if (_NumberOfPoints == 0) return;

  // Image resolution, i.e., length of voxel diagonal
  const double res = sqrt(pow(_Image->XSize(), 2) +
                          pow(_Image->YSize(), 2) +
                          pow(_Image->ZSize(), 2));

  // Parameters for ray casting to sample image intensities near surface
  _StepLength = .25 * res;
  if (_MaxDistance <= 0.) _MaxDistance = 4. * res;

  // Add point data arrays
  AddPointData("Distance");
  AddPointData("Magnitude");

  // Calculate image intensity statistics
  _LocalWhiteMatterMean.Clear();
  _LocalWhiteMatterVariance.Clear();
  _LocalGreyMatterMean.Clear();
  _LocalGreyMatterVariance.Clear();
  if (_EdgeType == NeonatalWhiteSurface) {
    ImageAttributes attr = _Image->Attributes(); attr._dt = 0.;
    if (_WhiteMatterMask) {
      if (!_WhiteMatterMask->HasSpatialAttributesOf(_Image)) {
        Throw(ERR_RuntimeError, __FUNCTION__, "Attributes of white matter mask differ from those of the intensity image!");
      }
      ComputeGlobalStatistics global;
      ParallelForEachVoxel(attr, _Image, _WhiteMatterMask, global);
      _GlobalWhiteMatterMean     = global.Mean();
      _GlobalWhiteMatterVariance = global.Variance();
      if (_WhiteMatterWindowWidth > 0) {
        _LocalWhiteMatterMean.Initialize(attr);
        _LocalWhiteMatterVariance.Initialize(attr);
        ComputeLocalStatistics local(attr, _WhiteMatterWindowWidth, _GlobalWhiteMatterMean, _GlobalWhiteMatterVariance);
        ParallelForEachVoxel(attr, _Image, _WhiteMatterMask, &_LocalWhiteMatterMean, &_LocalWhiteMatterVariance, local);
      }
    }
    if (_GreyMatterMask) {
      if (!_GreyMatterMask->HasSpatialAttributesOf(_Image)) {
        Throw(ERR_RuntimeError, __FUNCTION__, "Attributes of grey matter mask differ from those of the intensity image!");
      }
      ComputeGlobalStatistics global;
      ParallelForEachVoxel(attr, _Image, _GreyMatterMask, global);
      _GlobalGreyMatterMean     = global.Mean();
      _GlobalGreyMatterVariance = global.Variance();
      if (_GreyMatterWindowWidth > 0) {
        _LocalGreyMatterMean.Initialize(attr);
        _LocalGreyMatterVariance.Initialize(attr);
        ComputeLocalStatistics local(attr, _GreyMatterWindowWidth, _GlobalGreyMatterMean, _GlobalGreyMatterVariance);
        ParallelForEachVoxel(attr, _Image, _GreyMatterMask, &_LocalGreyMatterMean, &_LocalGreyMatterVariance, local);
      }
    }
    if (IsNaN(_MinIntensity)) {
      _MinIntensity = _GlobalGreyMatterMean  - 5. * sqrt(_GlobalGreyMatterVariance);
    }
    if (IsNaN(_MaxIntensity)) {
      _MaxIntensity = _GlobalWhiteMatterMean + 5. * sqrt(_GlobalWhiteMatterVariance);
    }
  }
}

// =============================================================================
// Evaluation
// =============================================================================

// -----------------------------------------------------------------------------
void ImageEdgeDistance::Update(bool gradient)
{
  // Update base class
  SurfaceForce::Update(gradient);

  vtkPolyData  * const surface   = DeformedSurface();
  vtkDataArray * const distances = PointData("Distance");
  vtkDataArray * const magnitude = PointData("Magnitude");
  vtkDataArray * const status    = Status();

  if (distances->GetMTime() >= surface->GetMTime()) return;

  // Compute distance to closest image edge
  ContinuousImage image;
  image.Input(_Image);
  image.Initialize();

  MIRTK_START_TIMING();
  ComputeDistances eval;
  eval._Points       = Points();
  eval._Status       = InitialStatus();
  eval._Normals      = Normals();
  eval._Image        = &image;
  eval._Distances    = distances;
  eval._Magnitude    = magnitude;
  eval._Padding      = _Padding;
  eval._MinIntensity = _MinIntensity;
  eval._MaxIntensity = _MaxIntensity;
  eval._MinGradient  = _MinGradient;
  eval._MaxDistance  = _MaxDistance;
  eval._StepLength   = _StepLength;
  eval._EdgeType     = _EdgeType;

  eval._GlobalWhiteMatterMean     = _GlobalWhiteMatterMean;
  eval._GlobalWhiteMatterSigma    = sqrt(_GlobalWhiteMatterVariance);
  eval._GlobalWhiteMatterVariance = _GlobalWhiteMatterVariance;
  eval._GlobalGreyMatterMean      = _GlobalGreyMatterMean;
  eval._GlobalGreyMatterVariance  = _GlobalGreyMatterVariance;
  eval._LocalWhiteMatterMean      = (_LocalWhiteMatterMean    .IsEmpty() ? nullptr : &_LocalWhiteMatterMean);
  eval._LocalWhiteMatterVariance  = (_LocalWhiteMatterVariance.IsEmpty() ? nullptr : &_LocalWhiteMatterVariance);
  eval._LocalGreyMatterMean       = (_LocalGreyMatterMean     .IsEmpty() ? nullptr : &_LocalGreyMatterMean);
  eval._LocalGreyMatterVariance   = (_LocalGreyMatterVariance .IsEmpty() ? nullptr : &_LocalGreyMatterVariance);

  parallel_for(blocked_range<int>(0, _NumberOfPoints), eval);
  MIRTK_DEBUG_TIMING(5, "computing edge distances");

  // Smooth measurements
  if (_MedianFilterRadius > 0) {
    MIRTK_RESET_TIMING();
    MedianMeshFilter median;
    median.Input(surface);
    median.EdgeTable(SharedEdgeTable());
    median.Connectivity(_MedianFilterRadius);
    median.DataArray(distances);
    median.Run();
    distances->DeepCopy(median.Output()->GetPointData()->GetArray(distances->GetName()));
    MIRTK_DEBUG_TIMING(5, "edge distance median filtering");
  }
  if (_DistanceSmoothing > 0) {
    MIRTK_RESET_TIMING();
    MeshSmoothing smoother;
    smoother.Input(surface);
    smoother.EdgeTable(SharedEdgeTable());
    smoother.SmoothPointsOff();
    smoother.SmoothArray(distances->GetName());
    smoother.Weighting(MeshSmoothing::Gaussian);
    smoother.NumberOfIterations(_DistanceSmoothing);
    smoother.Run();
    distances->DeepCopy(smoother.Output()->GetPointData()->GetArray(distances->GetName()));
    MIRTK_DEBUG_TIMING(5, "edge distance smoothing");
  }
  if (_MagnitudeSmoothing > 0) {
    MIRTK_RESET_TIMING();
    MeshSmoothing smoother;
    smoother.Input(surface);
    smoother.EdgeTable(SharedEdgeTable());
    smoother.SmoothPointsOff();
    smoother.SmoothArray(magnitude->GetName());
    smoother.Weighting(MeshSmoothing::Combinatorial);
    smoother.NumberOfIterations(_MagnitudeSmoothing);
    smoother.Run();
    magnitude->DeepCopy(smoother.Output()->GetPointData()->GetArray(magnitude->GetName()));
    MIRTK_DEBUG_TIMING(5, "edge magnitude smoothing");
  }

  // Make force magnitude proportional to both edge distance and strength
  using mirtk::data::statistic::Mean;
  using mirtk::data::statistic::AbsPercentile;

  MIRTK_RESET_TIMING();
  bool * const mask = new bool[_NumberOfPoints];
  for (int ptId = 0; ptId < _NumberOfPoints; ++ptId) {
    mask[ptId] = (status->GetComponent(ptId, 0) != 0.);
  }
  const auto dmax = AbsPercentile::Calculate(95, distances, mask);
  const auto mavg = Mean::Calculate(magnitude, mask);
  delete[] mask;
  MIRTK_DEBUG_TIMING(5, "calculating edge distance statistics");

  if (dmax > 0. && mavg > 0.) {
    MIRTK_RESET_TIMING();
    ComputeMagnitude eval;
    eval._Status        = status;
    eval._Distances     = distances;
    eval._DistanceScale = 1. / max(.1, dmax);
    eval._MaxMagnitude  = mavg;
    eval._Magnitude     = magnitude;
    parallel_for(blocked_range<int>(0, _NumberOfPoints), eval);
    MIRTK_DEBUG_TIMING(5, "computing edge force magnitude");
  } else {
    magnitude->FillComponent(0, 0.);
  }

  distances->Modified();
  magnitude->Modified();
}

// -----------------------------------------------------------------------------
double ImageEdgeDistance::Evaluate()
{
  if (_NumberOfPoints == 0) return 0.;
  ComputePenalty eval;
  eval._Distances = PointData("Distance");
  parallel_reduce(blocked_range<int>(0, _NumberOfPoints), eval);
  return eval._Sum / _NumberOfPoints;
}

// -----------------------------------------------------------------------------
void ImageEdgeDistance::EvaluateGradient(double *gradient, double step, double weight)
{
  if (_NumberOfPoints == 0) return;

  memset(_Gradient, 0, _NumberOfPoints * sizeof(GradientType));

  ComputeGradient eval;
  eval._Normals   = Normals();
  eval._Magnitude = PointData("Magnitude");
  eval._Gradient  = _Gradient;
  parallel_for(blocked_range<int>(0, _NumberOfPoints), eval);

  SurfaceForce::EvaluateGradient(gradient, step, weight / _NumberOfPoints);
}


} // namespace mirtk
