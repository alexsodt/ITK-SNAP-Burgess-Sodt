#include "PolygonDrawingModel.h"
#include "PolygonScanConvert.h"
#include "SNAPOpenGL.h"
#include <iostream>
#include <cstdlib>
#include <algorithm>
#include <set>
#include <vnl/vnl_random.h>

#include <QApplication> // needed to read shift modifiers.. move this check to the UI later?
#include "GenericSliceModel.h"
#include "itkImage.h"
#include "itkPointSet.h"
#include "itkBSplineScatteredDataPointSetToImageFilter.h"
#include "IRISApplication.h"

#include <SNAPUIFlag.h>
#include <SNAPUIFlag.txx>

// Enable this model to be used with the flag engine
template class SNAPUIFlag<PolygonDrawingModel, PolygonDrawingUIState>;

using namespace std;


PolygonDrawingModel
::PolygonDrawingModel()
{
  m_CachedPolygon = false;
  m_State = INACTIVE_STATE;
  m_SelectedVertices = false;
  m_DraggingPickBox = false;
  m_StartX = 0; m_StartY = 0;
  m_PolygonSlice = PolygonSliceType::New();
  m_HoverOverFirstVertex = false;
  m_curSlice = -1;
  m_FreehandFittingRateModel = NewRangedConcreteProperty(8.0, 0.0, 100.0, 1.0);
  m_allPolygons = NULL;  
  // AJS slice index model.
  m_SliceIndexModel = wrapGetterSetterPairAsProperty(
        this,
        &Self::GetSliceIndexValueAndDomain,
        &Self::SetSliceIndexValue);

}

bool PolygonDrawingModel
::GetSliceIndexValueAndDomain(int &value, NumericValueRange<int> *domain)
{
	// I only want to set the value, not get it.
    return false;
}

// AJS: set the slice index.
void PolygonDrawingModel::SetSliceIndexValue(int value)
{
    if( m_curSlice != -1 )
    	SavePolygonSlice();
    m_curSlice = value;
//    cout << "get Slice index: " << value << endl ;
    LoadPolygonSlice( value );
}

PolygonDrawingModel
::~PolygonDrawingModel()
{

}

Vector2d PolygonDrawingModel::GetPixelSize()
{
  double vppr = m_Parent->GetSizeReporter()->GetViewportPixelRatio();
  Vector3d x =
    m_Parent->MapWindowToSlice(Vector2d(vppr)) -
    m_Parent->MapWindowToSlice(Vector2d(0.0));

  return Vector2d(x[0],x[1]);
}

bool
PolygonDrawingModel
::CheckNearFirstVertex(double x, double y, double pixel_x, double pixel_y)
{
  if(m_Vertices.size() > 2)
    {
    Vector2d A(m_Vertices.front().x / pixel_x,
               m_Vertices.front().y / pixel_y);
    Vector2d C(x / pixel_x, y / pixel_y);
    if((A-C).inf_norm() < 4)
      return true;
    }
  return false;
}

bool
PolygonDrawingModel
::ProcessPushEvent(double x, double y,
                   bool shift_state)
{
  bool handled = false;
  Vector2d pxsize = GetPixelSize();
  double pixel_x = pxsize(0), pixel_y = pxsize(1);

  if(m_State == INACTIVE_STATE)
    {
    SetState(DRAWING_STATE);
    m_Vertices.push_back( Vertex(x, y, false, true) );

    handled = true;
    }

  else if(m_State == DRAWING_STATE)
    {
    // Restart Dragging
    m_DragVertices.clear();

    // The hover state is false
    m_HoverOverFirstVertex = false;

    // Left click means to add a vertex to the polygon. However, for
    // compatibility reasons, we must make sure that there are no duplicates
    // in the polygon (otherwise, division by zero occurs).
    if(m_Vertices.size() == 0 ||
       m_Vertices.back().x != x || m_Vertices.back().y != y)
      {
      // Check if the user wants to close the polygon
      if(CheckNearFirstVertex(x, y, pixel_x, pixel_y))
        ClosePolygon();
      else
        m_Vertices.push_back( Vertex(x, y, false, true) );
      }

    handled = true;
    }

  else if(m_State == EDITING_STATE)
    {
    m_StartX = x;
    m_StartY = y;

    if(!shift_state && m_SelectedVertices &&
       (x >= (m_EditBox[0] - 4.0*pixel_x)) &&
       (x <= (m_EditBox[1] + 4.0*pixel_x)) &&
       (y >= (m_EditBox[2] - 4.0*pixel_y)) &&
       (y <= (m_EditBox[3] + 4.0*pixel_y)))
      {
      // user not holding shift key; if user clicked inside edit box,
      // edit box will be moved in drag event
      }
    else
      {
      if(!shift_state)
        {
        // clicked outside of edit box & shift not held, this means the
        // current selection will be cleared
        for(VertexIterator it = m_Vertices.begin(); it!=m_Vertices.end(); ++it)
          it->selected = false;
        m_SelectedVertices = false;
        }

      // check if vertex clicked
      if(CheckClickOnVertex(x,y,pixel_x,pixel_y,4))
        ComputeEditBox();

      // check if clicked near a line segment
      else if(CheckClickOnLineSegment(x,y,pixel_x,pixel_y,4))
        ComputeEditBox();

      // otherwise start dragging pick box
      else
        {
        m_DraggingPickBox = true;
        m_SelectionBox[0] = m_SelectionBox[1] = x;
        m_SelectionBox[2] = m_SelectionBox[3] = y;
        }
      }

    handled = true;
    }

  if(handled)
    InvokeEvent(StateMachineChangeEvent());

  return handled;
}

bool
PolygonDrawingModel
::ProcessMouseMoveEvent(double x, double y)
{
  if(m_State == DRAWING_STATE)
    {
    // Check if we are hovering over the starting vertex
    Vector2d pxsize = GetPixelSize();
    double pixel_x = pxsize(0), pixel_y = pxsize(1);
    bool hover = CheckNearFirstVertex(x, y, pixel_x, pixel_y);

    if(hover != m_HoverOverFirstVertex)
      {
      m_HoverOverFirstVertex = hover;
      return true;
      }
    }

  return false;
}

bool
PolygonDrawingModel
::ProcessDragEvent(double x, double y)
{
  bool handled = false;
  if(m_State == DRAWING_STATE)
    {
    if(m_Vertices.size() == 0)
      {
      m_Vertices.push_back(Vertex(x,y,false,true));
      }
    else
      {
      // Check/set the hover state
      ProcessMouseMoveEvent(x, y);

      // Check if a point should be added here
      if(this->GetFreehandFittingRate() == 0)
        {
        m_Vertices.push_back(Vertex(x,y,false,false));
        }
      else
        {
        Vector2d pxsize = GetPixelSize();
        Vertex &v = m_Vertices.back();
        double dx = (v.x-x) / pxsize[0];
        double dy = (v.y-y) / pxsize[1];
        double d = dx*dx+dy*dy;
        if(d >= this->GetFreehandFittingRate() * this->GetFreehandFittingRate())
          m_Vertices.push_back(Vertex(x,y,false,true));
        }
      }
    handled = true;
    }

  else if(m_State == EDITING_STATE)
    {
    if (m_DraggingPickBox)
      {
      m_SelectionBox[1] = x;
      m_SelectionBox[3] = y;
      }
    else
      {
      m_EditBox[0] += (x - m_StartX);
      m_EditBox[1] += (x - m_StartX);
      m_EditBox[2] += (y - m_StartY);
      m_EditBox[3] += (y - m_StartY);

      // If the selection is bounded by control vertices, we simply shift it
      for(VertexIterator it = m_Vertices.begin(); it!=m_Vertices.end(); ++it)
        {
        if (it->selected)
          {
          it->x += (x - m_StartX);
          it->y += (y - m_StartY);
          }
        }

      // If the selection is bounded by freehand vertices, we apply a smooth
      m_StartX = x;
      m_StartY = y;
      }

    handled = true;
    }

  if(handled)
    InvokeEvent(StateMachineChangeEvent());
  return handled;
}

bool
PolygonDrawingModel
::ProcessReleaseEvent(double x, double y)
{
  bool handled = false;
  Vector2d pxsize = GetPixelSize();
  double pixel_x = pxsize(0), pixel_y = pxsize(1);

  if(m_State == DRAWING_STATE)
    {
    // Check if we've closed the loop
    if(CheckNearFirstVertex(x, y, pixel_x, pixel_y))
      {
      ClosePolygon();
      }

    // Make sure the last point is a control point
    if(m_Vertices.size() && m_Vertices.back().control == false)
      m_Vertices.back().control = true;

    handled = true;
    }

  else if(m_State == EDITING_STATE)
    {
    if (m_DraggingPickBox)
      {
      m_DraggingPickBox = false;

      double temp;
      if (m_SelectionBox[0] > m_SelectionBox[1])
        {
        temp = m_SelectionBox[0];
        m_SelectionBox[0] = m_SelectionBox[1];
        m_SelectionBox[1] = temp;
        }
      if (m_SelectionBox[2] > m_SelectionBox[3])
        {
        temp = m_SelectionBox[2];
        m_SelectionBox[2] = m_SelectionBox[3];
        m_SelectionBox[3] = temp;
        }

      for(VertexIterator it = m_Vertices.begin(); it!=m_Vertices.end(); ++it)
        {
        if((it->x >= m_SelectionBox[0]) && (it->x <= m_SelectionBox[1])
           && (it->y >= m_SelectionBox[2]) && (it->y <= m_SelectionBox[3]))
          it->selected = 1;
        }
      ComputeEditBox();
      }
    handled = true;
    }

  if(handled)
    InvokeEvent(StateMachineChangeEvent());
  return handled;

}



/**
 * ComputeEditBox()
 *
 * purpose:
 * compute the bounding box around selected vertices
 *
 * post:
 * if m_Vertices are selected, sets m_SelectedVertices to 1, else 0
 */
void
PolygonDrawingModel
::ComputeEditBox()
{
  VertexIterator it;

  // Find the first selected vertex and initialize the selection box
  m_SelectedVertices = false;
  for (it = m_Vertices.begin(); it!=m_Vertices.end();++it)
    {
    if (it->selected)
      {
      m_EditBox[0] = m_EditBox[1] = it->x;
      m_EditBox[2] = m_EditBox[3] = it->y;
      m_SelectedVertices = true;
      break;
      }
    }

  // Continue only if a selection exists
  if (!m_SelectedVertices) return;

  // Grow selection box to fit all selected vertices
  for(it = m_Vertices.begin(); it!=m_Vertices.end();++it)
    {
    if (it->selected)
      {
      if (it->x < m_EditBox[0]) m_EditBox[0] = it->x;
      else if (it->x > m_EditBox[1]) m_EditBox[1] = it->x;

      if (it->y < m_EditBox[2]) m_EditBox[2] = it->y;
      else if (it->y > m_EditBox[3]) m_EditBox[3] = it->y;
      }
    }
}

void
PolygonDrawingModel
::DropLastPoint()
{
  if(m_State == DRAWING_STATE)
    {
    if(m_Vertices.size())
      m_Vertices.pop_back();
    InvokeEvent(StateMachineChangeEvent());
    }
}

void
PolygonDrawingModel
::ClosePolygon()
{
  if(m_State == DRAWING_STATE)
    {
    SetState(EDITING_STATE);
    m_SelectedVertices = true;

    for(VertexIterator it = m_Vertices.begin(); it!=m_Vertices.end(); ++it)
      it->selected = false;

    ComputeEditBox();
    InvokeEvent(StateMachineChangeEvent());
    }
}

/**
 * Delete()
 *
 * purpose:
 * delete all vertices that are selected
 *
 * post:
 * if all m_Vertices removed, m_State becomes INACTIVE_STATE
 * length of m_Vertices array does not decrease
 */
void
PolygonDrawingModel
::DeleteSelected()
{
  VertexIterator it=m_Vertices.begin();
  while(it!=m_Vertices.end())
    {
    if(it->selected)
      it = m_Vertices.erase(it);
    else ++it;
    }

  if (m_Vertices.empty())
    {
    SetState(INACTIVE_STATE);
    m_SelectedVertices = false;
    }

  ComputeEditBox();
  InvokeEvent(StateMachineChangeEvent());
}

void
PolygonDrawingModel
::Reset()
{
  SetState(INACTIVE_STATE);
  m_Vertices.clear();
  ComputeEditBox();
  InvokeEvent(StateMachineChangeEvent());
}

/**
 * Insert()
 *
 * purpose:
 * insert vertices between adjacent selected vertices
 *
 * post:
 * length of m_Vertices array does not decrease
 */
void
PolygonDrawingModel
::SplitSelectedEdges()
{
  // Insert a vertex between every pair of adjacent vertices
  VertexIterator it = m_Vertices.begin();
  while(it != m_Vertices.end())
    {
    // Get the itNext iterator to point to the next point in the list
    VertexIterator itNext = it;
    if(++itNext == m_Vertices.end())
      itNext = m_Vertices.begin();

    // Check if the insertion is needed
    if(it->selected && itNext->selected)
      {
      // Insert a new vertex
      Vertex vNew(0.5 * (it->x + itNext->x), 0.5 * (it->y + itNext->y), true, true);
      it = m_Vertices.insert(++it, vNew);
      }

    // On to the next point
    ++it;
    }
  InvokeEvent(StateMachineChangeEvent());
}

int
PolygonDrawingModel
::GetNumberOfSelectedSegments()
{
  int isel = 0;
  for(VertexIterator it = m_Vertices.begin(); it != m_Vertices.end(); it++)
    {
    // Get the itNext iterator to point to the next point in the list
    VertexIterator itNext = it;
    if(++itNext == m_Vertices.end())
      itNext = m_Vertices.begin();

    // Check if the insertion is needed
    if(it->selected && itNext->selected)
      isel++;
    }
  return isel;
}

void
PolygonDrawingModel
::ProcessFreehandCurve()
{
  // Special case: no fitting
  if(this->GetFreehandFittingRate() == 0.0)
    {
    for(VertexIterator it = m_DragVertices.begin();
      it != m_DragVertices.end(); ++it)
      {
      m_Vertices.push_back(*it);
      }
    m_DragVertices.clear();
    return;
    }

  // We will fit a b-spline of the 0-th order to the freehand curve
  if(m_Vertices.size() > 0)
    {
    // Prepend the last vertex before freehand drawing
    m_DragVertices.push_front(m_Vertices.back());
    m_Vertices.pop_back();
    }

  // Create a list of input points
  typedef itk::Vector<double, 2> VectorType;
  typedef itk::Image<VectorType, 1> ImageType;
  typedef itk::PointSet<VectorType, 1> PointSetType;
  PointSetType::Pointer pointSet = PointSetType::New();

  double len = 0;
  double t = 0, dt = 1.0 / (m_DragVertices.size());
  size_t i = 0;
  Vertex last;
  for(VertexIterator it = m_DragVertices.begin();
    it != m_DragVertices.end(); ++it)
    {
    PointSetType::PointType point;
    point[0] = t;
    pointSet->SetPoint(i,point);
    VectorType v;
    v[0] = it->x; v[1] = it->y;
    pointSet->SetPointData(i, v);
    t+=dt; i++;
    if(it != m_DragVertices.begin())
      {
      double dx = last.x - it->x;
      double dy = last.y - it->y;
      len += sqrt(dx * dx + dy * dy);
      }
    last = *it;
    }

  // Compute the number of control points
  size_t nctl = (size_t)ceil(len / this->GetFreehandFittingRate());
  if(nctl < 3)
    nctl = 3;

  // Compute the number of levels and the control points at coarsest level
  size_t nl = 1; size_t ncl = nctl;
  while(ncl >= 8)
    { ncl >>= 1; nl++; }


  // Create the scattered interpolator
  typedef itk::BSplineScatteredDataPointSetToImageFilter<
    PointSetType, ImageType> FilterType;
  FilterType::Pointer filter = FilterType::New();

  ImageType::SpacingType spacing; spacing.Fill( 0.001 );
  ImageType::SizeType size; size.Fill((int)(1.0/spacing[0]));
  ImageType::PointType origin; origin.Fill(0.0);

  filter->SetSize( size );
  filter->SetOrigin( origin );
  filter->SetSpacing( spacing );
  filter->SetInput( pointSet );
  filter->SetSplineOrder( 1 );
  FilterType::ArrayType ncps;
  ncps.Fill(ncl);
  filter->SetNumberOfLevels(nl);
  filter->SetNumberOfControlPoints(ncps);
  filter->SetGenerateOutputImage(false);

  // Run the filter
  filter->Update();

  ImageType::Pointer lattice = filter->GetPhiLattice();
  size_t n = lattice->GetBufferedRegion().GetNumberOfPixels();
  for(size_t i = 0; i < n; i++)
    {
    ImageType::IndexType idx;
    idx.Fill(i);
    VectorType v = lattice->GetPixel(idx);
    m_Vertices.push_back(Vertex(v[0],v[1],false,true));
    }

  /*

  // Get the control points?
  double du = 1.0 / nctl;
  for(double u = 0; u < 1.00001; u += du)
    {
    if(u > 1.0) u = 1.0;
    PointSetType::PointType point;
    point[0] = u;
    VectorType v;
    filter->Evaluate(point,v);
    m_Vertices.push_back(Vertex(v[0],v[1],false));
    }
    */

  // Empty the drag list
  // m_DragVertices.clear();
}

bool PolygonVertexTest(const PolygonVertex &v1, const PolygonVertex &v2)
{
  return v1.x == v2.x && v1.y == v2.y;
}

/**
 * CopyPolygon()
 *
 * purpose:
 * copies current polygon into the polygon m_Cache
 *
 *
 * pre:
 * buffer array has size width*height*4
 * m_State == EDITING_STATE || DRAWING_STATE
 * 
 * post:
 * m_State == same
 */
void
PolygonDrawingModel
::CopyPolygon()
{
  assert(m_State == EDITING_STATE || m_State == DRAWING_STATE );

  // Copy polygon into polygon m_Cache
  m_CachedPolygon = true;
  m_Cache = m_Vertices;


  // Set the state
//  SetState(INACTIVE_STATE);
//  InvokeEvent(StateMachineChangeEvent());

}

/**
 * RevertPolygon()
 *
 * purpose:
 * copies current polygon into the polygon m_Cache
 *
 *
 * pre:
 * buffer array has size width*height*4
 * m_State == EDITING_STATE || DRAWING_STATE
 * 
 * post:
 * m_State == same
 */
void
PolygonDrawingModel
::RevertPolygon()
{
//  assert(m_State == EDITING_STATE || m_State == DRAWING_STATE );

  // Copy polygon into polygon m_Cache

  SavedSlicePolygons *gotIt = NULL;
  for( SavedSlicePolygons *aSlice = m_allPolygons; aSlice; aSlice = aSlice->next )
  {
	if( aSlice->slice == m_curSlice )
	{	
		gotIt = aSlice;
		break;
	}
  }   
	
	if( !gotIt )
	{
		gotIt = new SavedSlicePolygons;
		gotIt->theState =m_State;
		gotIt->next = m_allPolygons;
		gotIt->slice = m_curSlice;
		m_allPolygons = gotIt;
	}

	
   m_Vertices = gotIt->accepted_vertices; 

  // Set the state
//  SetState(INACTIVE_STATE);
//  InvokeEvent(StateMachineChangeEvent());

}

/**
 * AcceptPolygon()
 *
 * purpose:
 * to rasterize the current polygon into a buffer & copy the edited polygon
 * into the polygon m_Cache
 *
 * parameters:
 * buffer - an array of unsigned chars interpreted as an RGBA buffer
 * width  - the width of the buffer
 * height - the height of the buffer
 *
 * pre:
 * buffer array has size width*height*4
 * m_State == EDITING_STATE
 *
 * post:
 * m_State == INACTIVE_STATE
 */
void
PolygonDrawingModel
::AcceptPolygon(std::vector<IRISWarning> &warnings)
{
  assert(m_State == EDITING_STATE);

  // Allocate the polygon to match current image size. This will only
  // allocate new memory if the slice size changed
  itk::Size<2> sz = {{ (itk::SizeValueType) m_Parent->GetSliceSize()[0],
                       (itk::SizeValueType) m_Parent->GetSliceSize()[1] }};
  m_PolygonSlice->SetRegions(sz);
  m_PolygonSlice->Allocate();

  // Remove duplicates from the vertex array
  VertexIterator itEnd = std::unique(m_Vertices.begin(), m_Vertices.end(), PolygonVertexTest);
  m_Vertices.erase(itEnd, m_Vertices.end());

  // There may still be duplicates in the array, in which case we should
  // add a tiny offset to them. Thanks to Jeff Tsao for this bug fix!
  std::set< std::pair<double, double> > xVertexSet;
  vnl_random rnd;
  for(VertexIterator it = m_Vertices.begin(); it != m_Vertices.end(); ++it)
    {
    while(xVertexSet.find(make_pair(it->x, it->y)) != xVertexSet.end())
      {
      it->x += 0.0001 * rnd.drand32(-1.0, 1.0);
      it->y += 0.0001 * rnd.drand32(-1.0, 1.0);
      }
    xVertexSet.insert(make_pair(it->x, it->y));
    }

  // Scan convert the points into the slice
  typedef PolygonScanConvert<PolygonSliceType, float, VertexIterator> ScanConvertType;

  ScanConvertType::RasterizeFilled(
    m_Vertices.begin(), m_Vertices.size(), m_PolygonSlice);

  // Apply the segmentation to the main segmentation
  int nUpdates = m_Parent->MergeSliceSegmentation(m_PolygonSlice);
  if(nUpdates == 0)
    {
    warnings.push_back(
          IRISWarning("Warning: No voxels updated."
                      "No voxels in the segmentation image were changed as the "
                      "result of accepting this polygon. Check that the foreground "
                      "and background labels are set correctly."));
    }

    SaveTempToAccepted();

#if 0 // In Burgess-lab mode, if we accept the polygon it doesn't get removed.
  // Copy polygon into polygon m_Cache
  m_CachedPolygon = true;
  m_Cache = m_Vertices;

  // Reset the vertex array for next time
  m_Vertices.clear();
  m_SelectedVertices = false;

  // Set the state
  SetState(INACTIVE_STATE);
  InvokeEvent(StateMachineChangeEvent());

  ClearPolygonSlice(m_curSlice);
#endif
}

/**
 * PastePolygon()
 *
 * purpose:
 * copy the m_Cached polygon to the edited polygon
 *
 * pre:
 * m_CachedPolygon == 1
 * m_State == INACTIVE_STATE
 *
 * post:
 * m_State == EDITING_STATE
 */
void
PolygonDrawingModel
::PastePolygon(void)
{
  // Copy the cache into the vertices
  m_Vertices = m_Cache;

  // Select everything
  for(VertexIterator it = m_Vertices.begin(); it!=m_Vertices.end();++it)
    it->selected = false;

  // Set the state
  m_SelectedVertices = false;
  SetState(EDITING_STATE);

  // Compute the edit box
  ComputeEditBox();
  InvokeEvent(StateMachineChangeEvent());
}


/**
 * Check if a click is within k pixels of a vertex, if so select the vertices
 * of that line segment
 */
bool
PolygonDrawingModel
::CheckClickOnVertex(double x, double y, double pixel_x, double pixel_y, int k)
{
  // check if clicked within 4 pixels of a node (use closest node)
  VertexIterator itmin = m_Vertices.end();
  double distmin = k;
  for(VertexIterator it = m_Vertices.begin(); it!=m_Vertices.end(); ++it)
    {
    Vector2d A(it->x / pixel_x, it->y / pixel_y);
    Vector2d C(x / pixel_x, y / pixel_y);
    double dist = (A-C).inf_norm();

    if(distmin > dist)
      {
      distmin = dist;
      itmin = it;
      }
    }

  if(itmin != m_Vertices.end())
    {
    itmin->selected = true;
    return true;
    }
  else return false;
}

/**
 * Check if a click is within k pixels of a line segment, if so select the vertices
 * of that line segment
 */
bool
PolygonDrawingModel
::CheckClickOnLineSegment(
  double x, double y, double pixel_x, double pixel_y, int k)
{
  // check if clicked near a line segment
  VertexIterator itmin1 = m_Vertices.end(), itmin2 = m_Vertices.end();
  double distmin = k;
  for(VertexIterator it = m_Vertices.begin(); it!=m_Vertices.end(); ++it)
    {
    VertexIterator itnext = it;
    if(++itnext == m_Vertices.end())
      itnext = m_Vertices.begin();

    Vector2d A(it->x / pixel_x, it->y / pixel_y);
    Vector2d B(itnext->x / pixel_x, itnext->y / pixel_y);
    Vector2d C(x / pixel_x, y / pixel_y);

    double ab = (A - B).squared_magnitude();
    if(ab > 0)
      {
      double alpha = - dot_product(A-B, B-C) / ab;
      if(alpha > 0 && alpha < 1)
        {
        double dist = (alpha * A + (1-alpha) * B - C).magnitude();
        if(distmin > dist)
          {
          distmin = dist;
          itmin1 = it;
          itmin2 = itnext;
          }
        }
      }
    }

  if(itmin1 != m_Vertices.end())
    {
    itmin1->selected = true;
    itmin2->selected = true;
    return true;
    }
  else return false;
}

/* Can the polygon be closed? */
bool
PolygonDrawingModel
::CanClosePolygon()
{
  return m_Vertices.size() > 2;
}

/* Can last point be dropped? */
bool
PolygonDrawingModel
::CanDropLastPoint()
{
  return m_Vertices.size() > 0;
}

/* Can edges be split? */
bool
PolygonDrawingModel
::CanInsertVertices()
{
  return GetNumberOfSelectedSegments() > 0;
}

void PolygonDrawingModel::SetState(PolygonDrawingModel::PolygonState state)
{
  if(m_State != state)
    {
    m_State = state;
    InvokeEvent(StateMachineChangeEvent());
    }
}

bool PolygonDrawingModel::CheckState(PolygonDrawingUIState state)
{
  switch(state)
    {
    case UIF_HAVE_VERTEX_SELECTION:
      return this->GetSelectedVertices();
    case UIF_HAVE_EDGE_SELECTION:
      return this->CanInsertVertices();
    case UIF_INACTIVE:
      return m_State == INACTIVE_STATE;
    case UIF_DRAWING:
      return m_State == DRAWING_STATE;
    case UIF_EDITING:
      return m_State == EDITING_STATE;
    case UIF_HAVEPOLYGON:
      return m_Vertices.size() > 0;
    case UIF_HAVECACHED:
      return m_CachedPolygon;
    }

  return false;
}


/**
 * LoadPolygonSlice()
 *
 * AJS edit the pre/post below.
 *
 * purpose:
 * copy the saved slice polygon to the current polygon
 *
 * pre:
 * m_CachedPolygon == 1
 * m_State == INACTIVE_STATE
 *
 * post:
 * m_State == EDITING_STATE
 */
void
PolygonDrawingModel::LoadPolygonSlice( int slice )
{
  // find the vertices for this slice.
 
  int got_it = 0;

  for( SavedSlicePolygons *aSlice = m_allPolygons; aSlice; aSlice = aSlice->next )
  {
	if( aSlice->slice == slice )
	{	
                got_it = 1;
  		// Copy the cache into the vertices
		  m_Vertices = aSlice->temp_vertices;
		
		  int has_vertex = 0;
		  // Select everything
		  for(VertexIterator it = m_Vertices.begin(); it!=m_Vertices.end();++it)
		  {
			has_vertex = 1;
		    it->selected = false;
		  }

		if( has_vertex && aSlice->theState == EDITING_STATE )
		{
		  m_SelectedVertices = false;
		  SetState(EDITING_STATE);
		
		  // Compute the edit box
		  ComputeEditBox();
		}	
		else if( aSlice->theState == DRAWING_STATE )
		{
		  SetState(DRAWING_STATE);
		}	
/*
		if( has_vertex )
		{	
		  // Set the state
		  m_SelectedVertices = false;
		  SetState(EDITING_STATE);
		
		  // Compute the edit box
		  ComputeEditBox();
		}
		else
		{
			SetState(INACTIVE_STATE);
		}
*/		  
		InvokeEvent(StateMachineChangeEvent());

		break;
	}
  }
//	printf("Got_it: %d control_keys: %d.\n", got_it, (int)(QApplication::keyboardModifiers() & Qt::ControlModifier));
	
	if( !got_it && !(QApplication::keyboardModifiers() & Qt::ControlModifier) )
   	{
  		m_Vertices.clear();
 		ComputeEditBox();
  		if(m_State != INACTIVE_STATE) 
		{
			SetState(INACTIVE_STATE);
			InvokeEvent(StateMachineChangeEvent());
		}
	}

}


/**
 * ClearPolygonSlice()
 *
 * AJS edit the pre/post below.
 *
 * purpose:
 * clear the current polygon
 *
 */
void
PolygonDrawingModel
::ClearPolygonSlice( int slice )
{
  // find the vertices for this slice.

  SavedSlicePolygons *prev = NULL;
  for( SavedSlicePolygons *aSlice = m_allPolygons; aSlice; aSlice = aSlice->next )
  {
	if( aSlice->slice == slice )
	{	
		if( prev )
			prev->next = aSlice->next;
		else
			m_allPolygons = aSlice->next;

		delete aSlice;
		break;
	}
	prev = aSlice;
  }   

}


// AJS save polygons
void PolygonDrawingModel::WritePolygonsToBuffer( TiXmlElement *root )
//void PolygonDrawingModel::WritePolygonsToBuffer( QTextStream &out )
{
	SavePolygonSlice();

	TiXmlElement *view = new TiXmlElement("view");
	root->LinkEndChild(view);

	// write polygons to an ascii buffer for saving.

	int npoly = 0;

  	for( SavedSlicePolygons *aSlice = m_allPolygons; aSlice; aSlice = aSlice->next )
		npoly++;

	view->SetAttribute("npoly", npoly );

//	out << npoly << endl;

  	for( SavedSlicePolygons *aSlice = m_allPolygons; aSlice; aSlice = aSlice->next )
	{
		for( int pass = 0; pass < 2; pass++ )
		{
			// pass 0 is accepted, pass 1 is temp.

			TiXmlElement *slice = new TiXmlElement("slice");
			
			view->LinkEndChild(slice);
	
			slice->SetAttribute( "index", aSlice->slice );
			slice->SetAttribute( "theState", aSlice->theState );
			if( pass == 0 )
				slice->SetAttribute( "type", "accepted" );
			else
				slice->SetAttribute( "type", "temp" );

			VertexList *the_vertices;

			if( pass == 0 )
				the_vertices = &(aSlice->accepted_vertices);
			else
				the_vertices = &(aSlice->temp_vertices);
				
			int nv=0;
			for(VertexIterator it = the_vertices->begin(); it!= the_vertices->end();++it)
				nv++;
			slice->SetAttribute( "nv", nv );
			for(VertexIterator it = the_vertices->begin(); it!= the_vertices->end();++it)
			{
				TiXmlElement *vertex = new TiXmlElement("vertex");
				slice->LinkEndChild(vertex);
				vertex->SetAttribute( "x", it->x );
				vertex->SetAttribute( "y", it->y );
	
			}
		}
	}	
}

// AJS open polygons
void PolygonDrawingModel::ReadPolygonsFromBuffer( TiXmlElement *view )
{
	// clear the current list if it's here.

	SavedSlicePolygons *next = NULL;
	for( SavedSlicePolygons *aSlice = m_allPolygons; aSlice; aSlice = next )
	{
		next = aSlice->next;
		
		delete aSlice;
	}

	m_allPolygons = NULL;

	
	int npoly = 0;

	if( view->QueryIntAttribute("npoly", &npoly) != TIXML_SUCCESS)
	{
		printf("Failed to read npoly from saved polygons.\n");
		return;
	}

	TiXmlElement * poly = view->FirstChildElement("slice");

	if( poly )
	{
		int p = 0;

		while( poly )
		{
	
	
			int slice_index;
			if( poly->QueryIntAttribute("index", &(slice_index)) != TIXML_SUCCESS)
			{
				printf("Failed to read slice index from polygon.\n");
				return;
			}
			
			SavedSlicePolygons *aSlice = NULL;

			for( SavedSlicePolygons *t = m_allPolygons; t; t= t->next )
			{
				if( t->slice == slice_index )
					aSlice = t;
			}

			if( ! aSlice )
			{
				aSlice = new SavedSlicePolygons;
				aSlice->slice = slice_index;
	
				// add it to linked list.
				aSlice->next = m_allPolygons;
				m_allPolygons = aSlice;
			}

			int temp_state = 0;
			int type = 0;
			
			if( poly->QueryIntAttribute("theState", &(temp_state)) != TIXML_SUCCESS)
			{
			}
		
			string theType; 	

			VertexList *the_vertices = &(aSlice->accepted_vertices);

			if( poly->QueryStringAttribute("type", &(theType)) != TIXML_SUCCESS)
			{

			}
			else
			{
				if( theType == "temp" )
					the_vertices = &(aSlice->temp_vertices);
			}

			aSlice->theState = (PolygonState)temp_state;
	
			// read in file, same as write. nverts, read in verts..
			int nv;
			if( poly->QueryIntAttribute("nv", &(nv)) != TIXML_SUCCESS)
			{
				printf("Failed to read number of vertices from polygon.\n");
				return;
			}
	
			TiXmlElement * vert = poly->FirstChildElement("vertex");

	
			if( vert )
			{
				//for( int v = 0; v < nv; v++ )
				int v = 0;
		
				for( vert; vert; vert = vert->NextSiblingElement("vertex"), v++ )
				{
					double x=0,y=0;
		
					if( vert->QueryDoubleAttribute("x", &(x)) != TIXML_SUCCESS)
					{
						printf("Failed to read x coordinate from polygon vertex.\n");
					}
					if( vert->QueryDoubleAttribute("y", &(y)) != TIXML_SUCCESS)
					{
						printf("Failed to read y coordinate from polygon vertex.\n");
					}
		      			Vertex vNew( x, y, true, true);
		
					the_vertices->insert(the_vertices->end(), vNew);
				}	
			}

			poly = poly->NextSiblingElement("slice");
			p++;
		}
    	}

	LoadPolygonSlice( m_curSlice );
}

// this saves the current polygon into a linked list of records, one for each slice.

void PolygonDrawingModel::SaveTempToAccepted( void )
{
  SavedSlicePolygons *gotIt = NULL;
  for( SavedSlicePolygons *aSlice = m_allPolygons; aSlice; aSlice = aSlice->next )
  {
	if( aSlice->slice == m_curSlice )
	{	
		gotIt = aSlice;
		break;
	}
  }   
	
	if( !gotIt )
	{
		gotIt = new SavedSlicePolygons;
		gotIt->theState =m_State;
		gotIt->next = m_allPolygons;
		gotIt->slice = m_curSlice;
		m_allPolygons = gotIt;
	}

	
   gotIt->accepted_vertices = m_Vertices; 
}

void
PolygonDrawingModel
::SavePolygonSlice( void  )
{
  // find the vertices for this slice.

  
	if( m_Vertices.size() > 0 ) {

	  SavedSlicePolygons *gotIt = NULL;
	  for( SavedSlicePolygons *aSlice = m_allPolygons; aSlice; aSlice = aSlice->next )
	  {
		if( aSlice->slice == m_curSlice )
		{	
			gotIt = aSlice;
			break;
		}
	  }   
	
	  
		if( !gotIt )
		{
			gotIt = new SavedSlicePolygons;
			gotIt->theState =m_State;
			gotIt->next = m_allPolygons;
			gotIt->slice = m_curSlice;
			m_allPolygons = gotIt;
		}
			
		gotIt->temp_vertices = m_Vertices; 
	}
	else
	{
		// explicitly remove any saved polygon.
	  
		SavedSlicePolygons *prev = NULL;

		  for( SavedSlicePolygons *aSlice = m_allPolygons; aSlice; aSlice = aSlice->next )
		  {
			if( aSlice->slice == m_curSlice )
			{	
				if( prev )
					prev->next = aSlice->next;
				else
					m_allPolygons = aSlice->next;

				delete aSlice;
				break;
			}
			prev = aSlice;
		  }   
	}
}

