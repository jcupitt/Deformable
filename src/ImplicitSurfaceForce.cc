/*
 * Medical Image Registration ToolKit (MIRTK)
 *
 * Copyright 2013-2015 Imperial College London
 * Copyright 2013-2015 Andreas Schuh
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

#include "mirtk/ImplicitSurfaceForce.h"

#include "mirtk/Math.h"
#include "mirtk/Parallel.h"
#include "mirtk/DataStatistics.h"
#include "mirtk/ImplicitSurfaceUtils.h"

#include "vtkType.h"
#include "vtkPoints.h"
#include "vtkDataArray.h"


namespace mirtk {


// =============================================================================
// Auxiliary functions
// =============================================================================

namespace ImplicitSurfaceForceUtils {

// -----------------------------------------------------------------------------
/// Compute distance to implicit surface along normal direction
struct ComputeMinimumDistances
{
  ImplicitSurfaceForce *_Force;
  vtkPoints            *_Points;
  vtkDataArray         *_Distances;

  void operator ()(const blocked_range<int> &ptIds) const
  {
    double p[3];
    for (int ptId = ptIds.begin(); ptId != ptIds.end(); ++ptId) {
      _Points->GetPoint(ptId, p);
      _Distances->SetComponent(ptId, 0, _Force->Distance(p));
    }
  }
};

// -----------------------------------------------------------------------------
/// Compute distance to implicit surface along normal direction
struct ComputeNormalDistances
{
  ImplicitSurfaceForce *_Force;
  vtkPoints            *_Points;
  vtkDataArray         *_Normals;
  vtkDataArray         *_Distances;

  void operator ()(const blocked_range<int> &ptIds) const
  {
    double p[3], n[3];
    for (int ptId = ptIds.begin(); ptId != ptIds.end(); ++ptId) {
      _Points ->GetPoint(ptId, p);
      _Normals->GetTuple(ptId, n);
      _Distances->SetComponent(ptId, 0, _Force->Distance(p, n));
    }
  }
};


} // namespace ImplicitSurfaceForceUtils
using namespace ImplicitSurfaceForceUtils;

// =============================================================================
// Construction/Destruction
// =============================================================================

// -----------------------------------------------------------------------------
ImplicitSurfaceForce::ImplicitSurfaceForce(const char *name, double weight)
:
  SurfaceForce(name, weight),
  _DistanceMeasure(DM_Minimum),
  _Offset(.0),
  _MaxDistance(.0)
{
}

// -----------------------------------------------------------------------------
void ImplicitSurfaceForce::CopyAttributes(const ImplicitSurfaceForce &other)
{
  _DistanceMeasure = other._DistanceMeasure;
  _Offset          = other._Offset;
  _MaxDistance     = other._MaxDistance;
}

// -----------------------------------------------------------------------------
ImplicitSurfaceForce::ImplicitSurfaceForce(const ImplicitSurfaceForce &other)
:
  SurfaceForce(other)
{
  CopyAttributes(other);
}

// -----------------------------------------------------------------------------
ImplicitSurfaceForce &ImplicitSurfaceForce::operator =(const ImplicitSurfaceForce &other)
{
  if (this != &other) {
    SurfaceForce::operator =(other);
    CopyAttributes(other);
  }
  return *this;
}

// -----------------------------------------------------------------------------
ImplicitSurfaceForce::~ImplicitSurfaceForce()
{
}

// =============================================================================
// Configuration
// =============================================================================

// -----------------------------------------------------------------------------
bool ImplicitSurfaceForce::SetWithPrefix(const char *param, const char *value)
{
  if (strcmp(param, "Implicit surface distance measure") == 0) {
    return FromString(value, _DistanceMeasure);
  }
  if (strcmp(param, "Implicit surface distance offset") == 0) {
    return FromString(value, _Offset);
  }
  return SurfaceForce::SetWithPrefix(param, value);
}

// -----------------------------------------------------------------------------
bool ImplicitSurfaceForce::SetWithoutPrefix(const char *param, const char *value)
{
  if (strcmp(param, "Measure") == 0) {
    return FromString(value, _DistanceMeasure);
  }
  if (strcmp(param, "Offset") == 0) {
    return FromString(value, _Offset);
  }
  return SurfaceForce::SetWithoutPrefix(param, value);
}

// -----------------------------------------------------------------------------
ParameterList ImplicitSurfaceForce::Parameter() const
{
  ParameterList params = SurfaceForce::Parameter();
  InsertWithPrefix(params, "Measure", _DistanceMeasure);
  InsertWithPrefix(params, "Offset",  _Offset);
  return params;
}

// =============================================================================
// Evaluation
// =============================================================================

// -----------------------------------------------------------------------------
void ImplicitSurfaceForce::Initialize()
{
  // Initialize base class
  SurfaceForce::Initialize();

  // Initialize maximum distance
  if (_MaxDistance <= .0) {
    using data::statistic::MaxAbs;
    _MaxDistance = MaxAbs::Calculate(_Image->NumberOfVoxels(), _Image->Data());
  }

  // Initialize input image interpolators
  _Distance.Input(_Image);
  _Distance.Initialize();
  _DistanceGradient.Input(_Image);
  _DistanceGradient.Initialize();
}

// =============================================================================
// Surface distance
// =============================================================================

// -----------------------------------------------------------------------------
double ImplicitSurfaceForce::SelfDistance(const double p[3], const double n[3]) const
{
  return SurfaceForce::SelfDistance(p, n, _MaxDistance);
}

// -----------------------------------------------------------------------------
double ImplicitSurfaceForce::Distance(const double p[3]) const
{
  return ImplicitSurfaceUtils::Evaluate(_Distance, p, _Offset);
}

// -----------------------------------------------------------------------------
double ImplicitSurfaceForce::Distance(const double p[3], const double n[3]) const
{
  const double mind = ImplicitSurfaceUtils::Evaluate(_Distance, p, _Offset);
  double d = ImplicitSurfaceUtils::Distance(p, n, mind, .001, _MaxDistance, _Distance, _Offset);
  return copysign(d, mind);
}

// -----------------------------------------------------------------------------
void ImplicitSurfaceForce::DistanceGradient(const double p[3], double g[3], bool normalize) const
{
  ImplicitSurfaceUtils::Evaluate(_DistanceGradient, p, g, normalize);
}

// -----------------------------------------------------------------------------
vtkDataArray *ImplicitSurfaceForce::MinimumDistances() const
{
  return GetPointData("MinimumImplicitSurfaceDistance");
}

// -----------------------------------------------------------------------------
void ImplicitSurfaceForce::InitializeMinimumDistances()
{
  vtkDataArray *d = AddPointData("MinimumImplicitSurfaceDistance", 1, VTK_FLOAT, true);
  d->FillComponent(0, numeric_limits<double>::infinity());
}

// -----------------------------------------------------------------------------
void ImplicitSurfaceForce::UpdateMinimumDistances()
{
  return; // FIXME: Experimental update done in DeformableSurfaceModel::Update

  ComputeMinimumDistances eval;
  eval._Force     = this;
  eval._Points    = _PointSet->SurfacePoints();
  eval._Distances = MinimumDistances();
  parallel_for(blocked_range<int>(0, _NumberOfPoints), eval);
}

// -----------------------------------------------------------------------------
vtkDataArray *ImplicitSurfaceForce::NormalDistances() const
{
  return GetPointData("NormalImplicitSurfaceDistance");
}

// -----------------------------------------------------------------------------
void ImplicitSurfaceForce::InitializeNormalDistances()
{
  vtkDataArray *d = AddPointData("NormalImplicitSurfaceDistance", 1, VTK_FLOAT, true);
  d->FillComponent(0, _MaxDistance);
}

// -----------------------------------------------------------------------------
void ImplicitSurfaceForce::UpdateNormalDistances()
{
  return; // FIXME: Experimental update done in DeformableSurfaceModel::Update

  ComputeNormalDistances eval;
  eval._Force     = this;
  eval._Points    = _PointSet->SurfacePoints();
  eval._Normals   = _PointSet->SurfaceNormals();
  eval._Distances = Distances();
  parallel_for(blocked_range<int>(0, _NumberOfPoints), eval);
}

// -----------------------------------------------------------------------------
vtkDataArray *ImplicitSurfaceForce::Distances() const
{
  switch (_DistanceMeasure) {
    case DM_Minimum: return MinimumDistances();
    case DM_Normal:  return NormalDistances();
    default:
      cerr << "ImplicitSurfaceForce::Distances: Invalid distance measure type: " << _DistanceMeasure << endl;
      exit(1);
  }
}

// -----------------------------------------------------------------------------
void ImplicitSurfaceForce::InitializeDistances()
{
  switch (_DistanceMeasure) {
    case DM_Minimum: InitializeMinimumDistances(); break;
    case DM_Normal:  InitializeNormalDistances();  break;
    default:
      cerr << "ImplicitSurfaceForce::InitializeDistances: Invalid distance measure type: " << _DistanceMeasure << endl;
      exit(1);
  }
}

// -----------------------------------------------------------------------------
void ImplicitSurfaceForce::UpdateDistances()
{
  switch (_DistanceMeasure) {
    case DM_Minimum: UpdateMinimumDistances(); break;
    case DM_Normal:  UpdateNormalDistances();  break;
    default:
      cerr << "ImplicitSurfaceForce::UpdateDistances: Invalid distance measure type: " << _DistanceMeasure << endl;
      exit(1);
  }
}


} // namespace mirtk