/* GeomWindow.cxx */

#include <stddef.h>
#ifdef _WIN32
#include <windows.h>
#pragma warning(disable:4505)
#undef TRACE
#else
#include <unistd.h>
#endif
#ifdef OSX
#include <OpenGL/glu.h>
#else
#include <GL/glu.h>
#endif

#include <gtk/gtk.h>
#include <gtk/gtkgl.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <string>
#include "colormaps.h"
#include "dialogs.h"
#include "map3dmath.h"
#include "GeomWindow.h"
#include "Contour_Info.h"
#include "Map3d_Geom.h"
#include "Surf_Data.h"
#include "WindowManager.h"
#include "LegendWindow.h"
#include "PickWindow.h"
#include "MainWindow.h"
#include "ProcessCommandLineOptions.h"
#include "Transforms.h"
#include "BallMath.h"
#include "glprintf.h"
//#include "texture.h"
//#include "dot.h"
#include "eventdata.h"
#include "landmarks.h"
#include "lock.h"
#include "pickinfo.h"
#include "readfiles.h"
#include "reportstate.h"
#include "GeomWindowMenu.h"

extern Map3d_Info map3d_info;
extern GdkGLConfig *glconfig;
extern MainWindow *masterWindow;
//extern GtkWidget *fixed;
extern SaveDialog *savedialog;
extern int fstep;

#define CHAR_WIDTH .07
#define CHAR_HEIGHT .07
#define NUMTRIS 20

#include <algorithm>

using std::find;

// made a little bigger
GLuint selectbuffer[2048];


// init GeomWindow - creates a hidden gtk widget
GeomWindow::GeomWindow() : GLWindow(GEOMWINDOW, "Geometry Display",100,100)
{
  // things that need to be set before Init happens
  xmin = ymin = zmin = FLT_MAX;
  xmax = ymax = zmax = -FLT_MAX;
  clip = new Clip_Planes; //init the Clipping planes
#ifdef ROTATING_LIGHT
  Ball_Init(&light_pos, 1);
  Ball_Place(&light_pos, qOne, .8);
#endif

  dominantsurf = -1;
  secondarysurf = -1;
  vfov = 29.0f;
  fog1 = 1.8f;
  fog2 = 2.2f;
  bgcolor[0] = bgcolor[1] = bgcolor[2] = 0;
  fgcolor[0] = fgcolor[1] = fgcolor[2] = 1;
  showinfotext = 1;
  all_axes = 1;
  rgb_axes = true;
  modifiers = 0;
  idle_id = 0;
  showlocks = 1;

  setupEventHandlers();
}

void GeomWindow::setupEventHandlers()
{
  gtk_widget_set_events(drawarea, GDK_EXPOSURE_MASK | GDK_BUTTON_MOTION_MASK |
                        GDK_BUTTON_RELEASE_MASK | GDK_VISIBILITY_NOTIFY_MASK | GDK_BUTTON_PRESS_MASK |
                        GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);

  g_signal_connect(G_OBJECT(drawarea), "expose_event", G_CALLBACK(GeomWindowRepaint), this);
  g_signal_connect(G_OBJECT(drawarea), "configure_event", G_CALLBACK(GeomWindowReshape), this);
  g_signal_connect(G_OBJECT(drawarea), "button_press_event", G_CALLBACK(GeomWindowButtonPress), this);
  g_signal_connect(G_OBJECT(drawarea), "button_release_event", G_CALLBACK(GeomWindowButtonRelease), this);
  g_signal_connect(G_OBJECT(drawarea), "motion_notify_event", G_CALLBACK(GeomWindowMouseMotion), this);
 
  if (masterWindow == NULL) {
    g_signal_connect(G_OBJECT(window), "key_press_event", G_CALLBACK(GeomWindowKeyboardPress), this);
    g_signal_connect(G_OBJECT(window), "key_release_event", G_CALLBACK(GeomWindowKeyboardRelease), this);
    g_signal_connect_swapped(G_OBJECT(window), "delete_event", G_CALLBACK(map3d_quit), window);
    g_signal_connect(G_OBJECT(window), "window_state_event", G_CALLBACK (window_state_callback), NULL);
  }

}

//static
GeomWindow* GeomWindow::GeomWindowCreate(int _width, int _height, int _x, int _y, int def_width, int def_height)
{
  GeomWindow* win = map3d_info.geomwins[map3d_info.numGeomwins++];
  win->parentid = map3d_info.parentid;
  win->positionWindow(_width, _height, _x, _y, def_width, def_height);
  if (masterWindow && masterWindow->startHidden)
    gtk_widget_show(GTK_WIDGET(masterWindow->window));

  return win;
}

void GeomWindowInit(GtkWidget *, GdkEvent *, gpointer data)
{
  GeomWindow *priv = (GeomWindow *) data;
  if (!priv->standardInitialize())
    return;
  GeomBuildMenus(priv);
  // these two lines might be a little redundant, but when we create a new window,
  // and the window's creation is slow compared to the rest of the code, these things
  // might not be compensated for.
  GeomWindowUpdateAndRedraw(priv);

  priv->light_position[0] = 0.f;
  priv->light_position[1] = fabs(2 * priv->l2norm);
  priv->light_position[2] = 0.f;
  priv->light_position[3] = 1.f;

  gdk_gl_drawable_gl_begin(priv->gldrawable, priv->glcontext);

  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  glShadeModel(GL_FLAT);

  // these are probably initialized to true in meshlist.cc
  priv->node_pick_sphere.setActive(0);
  priv->node_lead_sphere.setActive(0);
  priv->node_lead.setValue(3, true);
  priv->lighting.setActive(6);

  glFogfv(GL_FOG_COLOR, priv->bgcolor);
  glFogi(GL_FOG_MODE, GL_LINEAR);
  glLightfv(GL_LIGHT0, GL_POSITION, priv->light_position);
  glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
  glEnable(GL_LIGHT0);
  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_TRUE);
  glEnable(GL_COLOR_MATERIAL);
  glDepthFunc(GL_LEQUAL);
  glPolygonOffset(1, 2);
  glDisable(GL_POLYGON_OFFSET_FILL);
  glDisable(GL_POLYGON_OFFSET_LINE);
  glDisable(GL_POLYGON_OFFSET_POINT);
  glSelectBuffer(2048, selectbuffer);
  glLineStipple(1, 57795);

  gdk_gl_drawable_gl_end(priv->gldrawable);
}

// for when a mesh is add, removed, or reloaded
void GeomWindow::recalcMinMax()
{
  xmin = ymin = zmin = FLT_MAX;
  xmax = ymax = zmax = -FLT_MAX;

  for (unsigned i = 0; i < meshes.size(); i++) {
    Mesh_Info* curmesh = meshes[i];
    xmin = MIN(xmin, curmesh->geom->xmin);
    ymin = MIN(ymin, curmesh->geom->ymin);
    zmin = MIN(zmin, curmesh->geom->zmin);
    xmax = MAX(xmax, curmesh->geom->xmax);
    ymax = MAX(ymax, curmesh->geom->ymax);
    zmax = MAX(zmax, curmesh->geom->zmax);
  }

  float xsize = xmax - xmin;
  float ysize = ymax - ymin;
  float zsize = zmax - zmin;
  xcenter = xsize / 2.f + xmin;
  ycenter = ysize / 2.f + ymin;
  zcenter = zsize / 2.f + zmin;
  l2norm = sqrt(xsize * xsize + ysize * ysize + zsize * zsize);


  /* Adjust clipping plane coordinates */
  clip->front_init = clip->front = zmax - zsize / 4;
  clip->back_init = clip->back = -zmin - zsize / 4;
  clip->step = zsize / 200;
  clip->frontmax = zmax + clip->step;
  clip->backmax = -zmin + clip->step;
}

void GeomWindow::addMesh(Mesh_Info *curmesh)
{
  /* copy from meshes to this window's mesh list */
  meshes.push_back(curmesh);
  curmesh->gpriv = this;
  recalcMinMax();

  // if this is the first mesh, copy vfov to the window and the 
  // rotation quaternion to the clipping planes (these could have
  // been set by the command line).
  if (meshes.size() == 1) {
    vfov = curmesh->mysurf->vfov;
    clip->bd.qNow = curmesh->tran->rotate.qNow;
    Qt_ToMatrix(Qt_Conj(clip->bd.qNow), clip->bd.mNow);
    Ball_EndDrag(&clip->bd);

    // also set up lock status on the first surf
    if (!map3d_info.lockgeneral) {
      dominantsurf = 0;
    }
    if (!map3d_info.lockframes || !map3d_info.lockrotate) {
      secondarysurf = 0;
    }

    // copy surf's bg/fg color
    bgcolor[0] = curmesh->mysurf->colour_bg[0] / 255.f;
    bgcolor[1] = curmesh->mysurf->colour_bg[1] / 255.f;
    bgcolor[2] = curmesh->mysurf->colour_bg[2] / 255.f;
    fgcolor[0] = curmesh->mysurf->colour_fg[0] / 255.f;
    fgcolor[1] = curmesh->mysurf->colour_fg[1] / 255.f;
    fgcolor[2] = curmesh->mysurf->colour_fg[2] / 255.f;
    large_font = (float)curmesh->mysurf->large_font;
    med_font = (float)curmesh->mysurf->med_font;
    small_font = (float)curmesh->mysurf->small_font; 
  }

  // make legend window's fg/bg equal to this one
  LegendWindow* lw = curmesh->legendwin;
  if (lw) {
    lw->bgcolor[0] = bgcolor[0];
    lw->bgcolor[1] = bgcolor[1];
    lw->bgcolor[2] = bgcolor[2];
    lw->fgcolor[0] = fgcolor[0];
    lw->fgcolor[1] = fgcolor[1];
    lw->fgcolor[2] = fgcolor[2];
  }
}

void GeomWindow::removeMesh(Mesh_Info *curmesh)
{
  Mesh_List::iterator iter = find(meshes.begin(), meshes.end(), curmesh);
  if (iter != meshes.end())
    meshes.erase(iter);
  recalcMinMax();
}

void GeomWindowReshape(GtkWidget *widget, GdkEvent * event, gpointer data)
{
  GeomWindow *priv = (GeomWindow *) data;
  if (!priv->ready)
    return;

  GeomWindowInit(widget, event, data);
  GdkEventConfigure* conf = (GdkEventConfigure*) event;

  priv->width = widget->allocation.width;
  priv->height = widget->allocation.height;

  // this is a window configure event, not a drawarea configure event
  if (widget == priv->window) {
    priv->x = conf->x;
    priv->y = conf->y;
    //gtk_widget_set_size_request(priv->drawarea, priv->width, priv->height);
    gtk_window_resize(GTK_WINDOW(priv->window), priv->width, priv->height);
    widget = priv->drawarea;
  }


  gdk_gl_drawable_gl_begin(priv->gldrawable, priv->glcontext);
  glViewport(0, 0, priv->width, priv->height);
  gdk_gl_drawable_gl_end(priv->gldrawable);

  gtk_widget_queue_draw(priv->drawarea);

}


void compute_mapping(Mesh_Info * mesh, Surf_Data * cursurf, float project_min, float project_max, float &a, float &b)
{
  float potmax = FLT_MAX, potmin = -FLT_MAX;
  float temp;

  cursurf->get_minmax(potmin, potmax);

  if (mesh->invert) {
    temp = potmax;
    potmax = potmin;
    potmin = temp;
  }
  //ComputeLinearMappingCoefficients(potmin, potmax, 0, mesh->cmap->max, a, b);
  a = (project_max - project_min) / (potmax - potmin);
  b = (project_min * potmax - potmin * project_max) / (potmax - potmin);
}

// del is false by default and represents whether picking is to delete a tri
// return true on a successful pick, false otherwise
bool Pick(GeomWindow * priv, int meshnum, int x, int y, bool del /*= false*/ )
{
  if (del && map3d_info.pickmode != TRIANGULATE_PICK_MODE)
    return false;
  int length = 0;
  int loop2;
  GLint viewport[4];
  int hits = 0;
  float **modelpts = 0;
  Mesh_Info *curmesh = 0;
  Map3d_Geom *curgeom = 0;
  Surf_Data *cursurf = 0;

  curmesh = priv->meshes[meshnum];
  cursurf = curmesh->data;
  curgeom = curmesh->geom;
  modelpts = curgeom->points[curgeom->geom_index];

  gdk_gl_drawable_gl_begin(priv->gldrawable, priv->glcontext);
  /* setup the transform for picking */
  Transform(curmesh, 0);
  glRenderMode(GL_SELECT);
  glGetIntegerv(GL_VIEWPORT, viewport);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();

  gluPickMatrix((double)x, (double)(y), map3d_info.picksize, map3d_info.picksize, viewport);
  gluPerspective(priv->vfov, priv->width / (float)priv->height, priv->l2norm, 3 * priv->l2norm);

  glInitNames();
  glPushName(meshnum);
  glPushName(1);

  // need to pick a triangle for these modes, not a node
  if (map3d_info.pickmode == FLIP_TRIANGLE_PICK_MODE || 
      map3d_info.pickmode == TRIANGLE_INFO_PICK_MODE || del) {
    length = curgeom->numelements;
    for (loop2 = 0; loop2 < length; loop2++) {
      glLoadName(loop2);
      glBegin(GL_TRIANGLES);

      int index = curgeom->elements[loop2][0];
      glVertex3f(modelpts[index][0], modelpts[index][1], modelpts[index][2]);
      index = curgeom->elements[loop2][1];
      glVertex3f(modelpts[index][0], modelpts[index][1], modelpts[index][2]);
      index = curgeom->elements[loop2][2];
      glVertex3f(modelpts[index][0], modelpts[index][1], modelpts[index][2]);

      glEnd();
    }

  }
  //pick a node
  else if (map3d_info.pickmode != EDIT_LANDMARK_PICK_MODE) {
    length = curgeom->numpts;
    for (loop2 = 0; loop2 < length; loop2++) {
      glLoadName(loop2);
      glBegin(GL_POINTS);
      glVertex3f(modelpts[loop2][0], modelpts[loop2][1], modelpts[loop2][2]);
      glEnd();
    }
  }
  //pick a landmark
  else {
    if (curgeom->landmarks) {
      length = curgeom->landmarks->numsegs;
      for (loop2 = 0; loop2 < length; loop2++) {
        glLoadName(loop2);
        glPushName(1);

        LandMarkSeg seg = curgeom->landmarks->segs[loop2];
        int seglength = seg.numpts;
        for (int loop3 = 0; loop3 < seglength; loop3++) {
          glLoadName(loop3);
          glBegin(GL_POINTS);
          glVertex3f(seg.pts[loop3][0], seg.pts[loop3][1], seg.pts[loop3][2]);
          glEnd();
        }
        glPopName();
      }
    }
    else {
      printf("No landmarks specified\n");
    }
  }
  glPopName();
  glPopName();

  int orig_hits = glRenderMode(GL_RENDER);
  hits = orig_hits;
  gdk_gl_drawable_gl_end(priv->gldrawable);

  GLuint* selection = selectbuffer;
  // note, the select buffer is organized in groups of pick records, as follows:
  // number of names
  // min depth value
  // max depth value
  // name list
  // to get to the next record, increment by 3+number of names

  if (orig_hits == 0) {
    printf("no hits, pick is unchanged.\n");
    return false;
  }
  else if (orig_hits == -1) {
    printf("Pick Buffer overflow.  Change the pick aperture, or reduce the\n");
    printf("number of pickable nodes (zoom in, or turn on clipping or shading)\n");
    return true;
  }
  else if (orig_hits > 1) {
    // don't allow geometry changing on multiple hits
    if (map3d_info.pickmode == TRIANGULATE_PICK_MODE || 
      map3d_info.pickmode == FLIP_TRIANGLE_PICK_MODE ||
      map3d_info.pickmode == EDIT_NODE_PICK_MODE ||
      map3d_info.pickmode == EDIT_LANDMARK_PICK_MODE ||
      map3d_info.pickmode == DELETE_NODE_PICK_MODE ||
      map3d_info.pickmode == EDIT_LANDMARK_PICK_MODE) {
      printf("Too many hits (%d).\n", hits);
      return true;
    }

    // for info modes we want to go through all picks instead of selecting one
    // otherwise, go through all records and pick the closest one
    if ( map3d_info.pickmode != INFO_PICK_MODE && map3d_info.pickmode != TRIANGLE_INFO_PICK_MODE) {
      GLuint* currentSelection = selectbuffer;
      for (int i = 0; i < orig_hits; i++) {
        int numNames = currentSelection[0];
        // see if our depth values are less (closer) than the current selection's
        if (currentSelection[1] < selection[1] && currentSelection[2] < selection[2])
          selection = currentSelection;
        currentSelection += 3+numNames;
      }
      // mark as only one hit
      hits = 1;
    }
  }

  for (int i = 0; i < hits; i++) {
    // process the selected hit(s)
    if (map3d_info.pickmode == NEW_WINDOW_PICK_MODE || map3d_info.pickmode == REFRESH_WINDOW_PICK_MODE) {
      if (map3d_info.numPickwins >= MAX_SURFS * 2 && map3d_info.pickmode == NEW_WINDOW_PICK_MODE) {
        printf("Already reached limit of time series windows\n");
        return true;
      }
      PickInfo *pick = new PickInfo;
      pick->type = 1;
      pick->show = 1;

      pick->mesh = priv->meshes[selection[3]];
      pick->rms = 0;

      pick->node = selection[4];
      pick->depth = (float)selection[1] / 0x7fffffff;

      if (orig_hits == 1) 
        printf("1 hit, pick: winid = %d, surf# = %d,"
                " node = %d, depth = %f\n", pick->mesh->gpriv->winid, pick->mesh->geom->surfnum, 
                pick->node, pick->depth);
      else
        printf("%d hits (using closest), pick: win# = %d, surf# = %d,"
                " node = %d, depth = %f\n", orig_hits, pick->mesh->gpriv->winid, 
                pick->mesh->geom->surfnum, pick->node, pick->depth);

      if (map3d_info.pickmode == REFRESH_WINDOW_PICK_MODE)
        map3d_info.useonepickwindow = true;
      else
        map3d_info.useonepickwindow = false;

      GeneratePick(pick);
    }
    //print the node's info to the display
    else if (map3d_info.pickmode == INFO_PICK_MODE) {
      float *location = curgeom->points[curgeom->geom_index][selection[4]];
      if (cursurf && cursurf->potvals) {
        printf("Surface #%ld, Node #%u, Channel #%ld, Value: %.2f, Frame #%ld\n", curgeom->surfnum,
	        selection[4] + 1, curmesh->geom->channels[selection[4] + 1],
	        cursurf->potvals[cursurf->framenum][selection[4]], cursurf->framenum + 1);
      }
      else {
        printf("Surface #%ld, Node #%u, Channel #%ld\n", curgeom->surfnum, selection[4] + 1,
	        curmesh->geom->channels[selection[4] + 1]);
      }
      printf("Location:  X: %f, Y: %f, Z: %f\n", location[0], location[1], location[2]);

    }
    //change reference
    else if (map3d_info.pickmode == REFERENCE_PICK_MODE) {
      float *location = curgeom->points[curgeom->geom_index][selection[4]];
      if (cursurf && cursurf->potvals) {
        printf("Surface #%ld, Node #%u, Channel #%ld, Value: %.2f, Frame #%ld\n", curgeom->surfnum,
	        selection[4] + 1,curmesh->geom->channels[selection[4] + 1],
	        cursurf->potvals[cursurf->framenum][selection[4]],cursurf->framenum + 1);
        cursurf->ChangeReference(selection[4] + 1, curgeom);
      }
      else{
        printf("Surface #%ld, Node #%u, Channel #%ld\n", curgeom->surfnum, selection[4] + 1,
	        curmesh->geom->channels[selection[4] + 1]);
      }
      printf("Location:  X: %f, Y: %f, Z: %f\n", location[0], location[1], location[2]);

    }
    //print triangle's info to display
    else if (map3d_info.pickmode == TRIANGLE_INFO_PICK_MODE) {
      printf("Surface #%2ld, Triangle #%4u, points: X        Y        Z\n", curgeom->surfnum, selection[4]);
      for (int i = 0; i < 3; i++) {
        int pointnum = curgeom->elements[selection[4]][i];
        float *location = curgeom->points[curgeom->geom_index][pointnum];
        printf("%35d:%4.3f, %4.3f, %4.3f\n", pointnum + 1, location[0], location[1], location[2]);
      }

    }
    //triangulate mode - pick three points and connect them as a triangle and add them to the geom
    else if (map3d_info.pickmode == TRIANGULATE_PICK_MODE && !del) {
      Triangulate(curmesh, selection[4]);
    }
    else if (map3d_info.pickmode == EDIT_NODE_PICK_MODE) {
      // reset the selected points in all meshes
      for (MeshIterator mi(0,0); !mi.isDone(); ++mi) {
        Mesh_Info* mesh = mi.getMesh();
        mesh->num_selected_pts = 0;
      }

      curmesh->selected_pts[0] = selection[4];
      curmesh->num_selected_pts = 1;
      Broadcast(MAP3D_REPAINT_ALL, 0);

    }
    else if (map3d_info.pickmode == DELETE_NODE_PICK_MODE) {
      DelNode(curmesh, selection[4]);
      //recalculate the contours for this action
      curmesh->cont->buildContours();
      for(unsigned i = 0; i<curmesh->fidConts.size();i++){      
        curmesh->fidConts[i]->buildContours();
      }
      for(unsigned i = 0; i<curmesh->fidMaps.size();i++){      
        curmesh->fidMaps[i]->buildContours();
      }
    }
    else if (map3d_info.pickmode == TRIANGULATE_PICK_MODE && del) {
      DelTriangle(curmesh, selection[4]);
      //recalculate the contours for this action
      curmesh->cont->buildContours();
      for(unsigned i = 0; i<curmesh->fidConts.size();i++){      
        curmesh->fidConts[i]->buildContours();
      }
      for(unsigned i = 0; i<curmesh->fidMaps.size();i++){      
        curmesh->fidMaps[i]->buildContours();
      }
    }
    else if (map3d_info.pickmode == EDIT_LANDMARK_PICK_MODE) {
      curmesh->landmarkdraw.picked_segnum = selection[4];
      curmesh->landmarkdraw.picked_ptnum = selection[5];
    }
    else if (map3d_info.pickmode == FLIP_TRIANGLE_PICK_MODE) {
      double temp = curmesh->geom->elements[selection[4]][2];
      curmesh->geom->elements[selection[4]][2] = (long)curmesh->geom->elements[selection[4]][1];
      curmesh->geom->elements[selection[4]][1] = (long)temp;
      ComputeTriNormals(curmesh->geom);
    }
    selection += selection[0] + 3;
  }
#if SHOW_OPENGL_ERRORS
  GLenum e = glGetError();
  if (e)
    printf("GeomWindow PickNode OpenGL Error: %s\n", gluErrorString(e));
#endif
  return true;
}

void DelTriangle(Mesh_Info * curmesh, int trinum)
{
  Map3d_Geom *geom = curmesh->geom;
  geom->numelements--;
  long *deleted = geom->elements[trinum];
  float *deletednorm = geom->fcnormals[trinum];
  int i;

  // fill the hole in the memory
  for (i = trinum; i < geom->numelements; i++) {
    geom->elements[i] = geom->elements[i + 1];
    geom->fcnormals[i] = geom->fcnormals[i + 1];
  }

  //preserve memory
  geom->elements[i] = deleted;
  geom->fcnormals[i] = deletednorm;

}

void DelNode(Mesh_Info * curmesh, int nodenum)
{
  Map3d_Geom *geom = curmesh->geom;
  float** pts = geom->points[geom->geom_index];

  Surf_Data *data = curmesh->data;
  geom->numpts--;
  if (data)
    data->numleads--;

  // -----------
  // move points greater than nodenum down (and their associated pots)
  float *deleted_node = pts[nodenum];
  float deleted_pot = 0;
  if (data)
    deleted_pot = data->potvals[0][nodenum];
  int i;

  for (i = nodenum; i < geom->numpts; i++) {
    pts[i] = pts[i + 1];
    geom->channels[i] = geom->channels[i + 1];
    if (data)
      data->potvals[0][i] = data->potvals[0][i + 1];
  }

  //preserve memory
  pts[i] = deleted_node;
  if (data)
    data->potvals[0][i] = deleted_pot;

  // -----------
  // delete all triangles that refer have nodenum, and have tris
  //   whose points are > nodenum point to the node before
  for (i = 0; i < geom->numelements; i++)
    for (int j = 0; j < 3; j++) {
      if (geom->elements[i][j] == nodenum) {
        DelTriangle(curmesh, i);
        i--;                    //compensate for all triangles being shifted down one
        break;
      }
      if (geom->elements[i][j] > nodenum) {
        geom->elements[i][j] = geom->elements[i][j] - 1;
      }
    }

  // recalculate scaling
  if (data) {
    data->MinMaxPot(geom);
    GlobalMinMax();
    FrameMinMax();

    // redraw legend window as well
    gtk_widget_queue_draw(curmesh->legendwin->drawarea);
  }
  
}


void Triangulate(Mesh_Info * curmesh, int nodenum)
{
  Map3d_Geom *geom = curmesh->geom;

  //see if picked node is already in triangle
  if ((curmesh->num_selected_pts == 1 && curmesh->selected_pts[0] == nodenum) ||
      (curmesh->num_selected_pts == 2 && (curmesh->selected_pts[0] == nodenum ||
                                          curmesh->selected_pts[1] == nodenum))) {
    printf("Picked Node is already part of triangle\n");
    return;
  }
  curmesh->selected_pts[curmesh->num_selected_pts] = nodenum;
  curmesh->num_selected_pts++;
  //triangulate them
  if (curmesh->num_selected_pts == 3) {
    curmesh->num_selected_pts = 0;

    //cases to allocate more memory
    if (geom->elements == NULL) {
      geom->elements = Alloc_lmatrix(NUMTRIS, 3);
      geom->elementsize = 3;
      geom->maxnumelts += 20;
    }
    else if (curmesh->geom->numelements + 1 > geom->maxnumelts) {
      geom->elements = Grow_lmatrix(geom->elements, geom->numelements, geom->numelements + NUMTRIS, 3, 3);
      geom->maxnumelts += 20;
    }
    geom->numelements++;
    for (int i = 0; i < 3; i++)
      geom->elements[geom->numelements - 1][i] = curmesh->selected_pts[i];
    ComputeTriNormals(geom);

    //recalculate the contours for this action
    curmesh->cont->buildContours();
    for(unsigned i = 0; i<curmesh->fidConts.size();i++){      
      curmesh->fidConts[i]->buildContours();
    }
    for(unsigned i = 0; i<curmesh->fidMaps.size();i++){      
      curmesh->fidMaps[i]->buildContours();
    }
  }
}

void GeneratePick(PickInfo * pick)
{
  Mesh_Info* mesh = pick->mesh;
  PickWindow *ppriv = 0;
  static PickWindow *top = 0;

  if (map3d_info.useonepickwindow)  // refresh pick window mode (only 1 window)
  {
    // make pick window only if it doesn't exist
    if (map3d_info.numPickwins == 0) { //used to be one per geom window - BJW
      mesh->pickstacktop = 0;

      // use default x,y pos
      ppriv = PickWindow::PickWindowCreate(-1, -1, -1, -1, 325, 140);
      ppriv->pick = pick;
      ppriv->mesh = mesh;
      ppriv->setPosAndShow();
      if (masterWindow)
        // call to resize children is so the children draw in a logical manner
        gtk_container_resize_children(GTK_CONTAINER(masterWindow->fixed));
      top = pick->pickwin = ppriv;
    }
    else {                      //if pick windows were created with the -at flag
      top = map3d_info.pickwins[map3d_info.numPickwins-1];
    }

    pick->pickwin = top;
    top->pick = pick;
    gtk_widget_queue_draw(top->drawarea);
    mesh->pickstack[mesh->pickstacktop] = pick;
  }
  else {
    mesh->pickstacktop++;
    mesh->pickstack[mesh->pickstacktop] = pick;

    // use default x,y pos
    ppriv = PickWindow::PickWindowCreate(-1, -1, -1, -1, 325, 140);
    ppriv->pick = pick;
    ppriv->mesh = mesh;
    ppriv->setPosAndShow();;
    if (masterWindow)
      // call to resize children is so the children draw in a logical manner
      gtk_container_resize_children(GTK_CONTAINER(masterWindow->fixed));
    pick->pickwin = ppriv;
    top = ppriv;
  }
}


void printMatrix16(float* matrix) {
  for (int i = 0; i < 16; i++) {
    printf("%f ", matrix[i]);
    if ((i % 4) == 3)
      printf("\n");
  }
  printf("\n");

}

//saves orientation of current mesh display, relative to relmesh
void SaveGeomToDisk(Mesh_Info * mesh, bool /*transform*/)
{
  //GeomWindow* priv = GET_GEOM_WINDOW_PRIVATE;
  //  Transforms *origtran = relmesh->tran;
  //Map3d_Geom *geom = mesh->geom;
  //GeomWindow *priv = mesh->gpriv;
  FILE *f;
  char filename[100];
  char basefilename[100];
  int append = 0;
  bool notdone = 1;
  char extension[10];

  //save pts file
  strcpy(basefilename, mesh->geom->basefilename);

  while (notdone) {
    append++;
    strcpy(filename, basefilename);
    StripExtension(filename);
    StripExtension(filename);
    sprintf(extension, ".%i.pts", append);
    strcat(filename, extension);
    if ((f = fopen(filename, "r")) == NULL) {
      break;
    }
    fclose(f);
  }
}

// takes a meshlist, an array of transforms as long as meshlist, and one filename
// should be called by SaveGeoms, a callback from the Save Dialog
void SaveMeshes(Mesh_List& ml, vector<bool> transforms, char* filename)
{
  std::string ext;
  ext = GetExtension(filename);

  if (ml.size() > 1 && ext != ".geom" && ext != ".mat") {
    char message[150];
    sprintf(message, "You can only save multiple surfaces to a .geom or .mat file");
    if (savedialog) {
      GtkWidget* err = gtk_message_dialog_new(GTK_WINDOW(savedialog->window), GTK_DIALOG_DESTROY_WITH_PARENT, 
                               GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, message);
      gtk_dialog_run(GTK_DIALOG(err));
      gtk_widget_destroy(err);
      return;
    }
  }

  if (ext != ".geom" && ext != ".fac" && ext != ".pts" && ext != ".mat") {
    char message[150];
    sprintf(message, "You can only save to a .geom, .mat, .pts, or .fac file");
    if (savedialog) {
      GtkWidget* err = gtk_message_dialog_new(GTK_WINDOW(savedialog->window), GTK_DIALOG_DESTROY_WITH_PARENT, 
                               GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, message);
      gtk_dialog_run(GTK_DIALOG(err));
      gtk_widget_destroy(err);
      return;
    }
  }
  // test for existence
  FILE* test = fopen(filename, "r");
  if (test) {
    char message[150];
    sprintf(message, "Overwrite %s?", filename);
    fclose(test);
    if (savedialog) {
      GtkWidget* overwrite = gtk_message_dialog_new(GTK_WINDOW(savedialog->window), GTK_DIALOG_DESTROY_WITH_PARENT, 
                               GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL, message);
      int response = gtk_dialog_run(GTK_DIALOG(overwrite));
      gtk_widget_destroy(overwrite);
      if (response != GTK_RESPONSE_OK)
        return;
      else
        KillFile(filename);
    }
  }
  

  int loop, loop2;
  // we need to make a list of all meshes to save in one call
  vector<Map3d_Geom*> geomlist;
  vector<float**> geom_oldpts;


  for (unsigned i = 0; i < ml.size(); i++) {

    Mesh_Info* mesh = ml[i];
    Map3d_Geom* geom = mesh->geom;
    GeomWindow* priv = mesh->gpriv;

    HMatrix mNow /*, original */ ;  // arcball rotation matrices
    Transforms *tran = mesh->tran;
    //translation matrix in column-major
    float centerM[4][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0}, 
    {-priv->xcenter,-priv->ycenter,-priv->zcenter,1}};
    float invCenterM[4][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0}, 
    {priv->xcenter,priv->ycenter,priv->zcenter,1}};
    float translateM[4][4] = { {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0},
    {tran->tx, tran->ty, tran->tz, 1}
    };
    float **rotated_pts = 0;

    float temp[16];
    float product[16];

    if (transforms[i]) {
      //rotation matrix
      Ball_Value(&tran->rotate, mNow);
      // apply translation
      // translate mesh's center to origin
      MultMatrix16x16((float *)translateM, (float *)invCenterM, (float*)product);
      // rotate
      MultMatrix16x16((float *)product, (float *)mNow, (float*)temp);
      // revert mesh translation to origin
      MultMatrix16x16((float*)temp, (float *) centerM, (float*)product);

      // array to copy the points into for geom files
      if (ext == ".geom" || ext == ".mat")
        rotated_pts = Alloc_fmatrix(geom->numpts, 3);
    }

    char ptsfilename[257];

    // if it is a fac or a pts we need to save a pts file, but with
    // fac, we should save a pts file under a different name
    if (ext == ".fac") {
      strcpy(ptsfilename, filename);
      StripExtension(ptsfilename);
      strcat(ptsfilename, ".pts");
    }
    else if (ext == ".pts")
      strcpy(ptsfilename, filename);

    // put file overwrite check here

    FILE* f = 0;
    if (ext == ".pts" || ext == ".fac")
      f = fopen(ptsfilename, "w");

    // we need to go through this loop for geom files too, to transform
    // its points
    float** pts = geom->points[geom->geom_index];
    for (loop = 0; loop < geom->numpts; loop++) {
      float rhs[4];
      float result[4];
      rhs[0] = pts[loop][0];
      rhs[1] = pts[loop][1];
      rhs[2] = pts[loop][2];
      rhs[3] = 1;

      if (transforms[i]) {
        MultMatrix16x4(product, rhs, result);
        if (ext == ".geom" || ext == ".mat") {
          rotated_pts[loop][0] = result[0];
          rotated_pts[loop][1] = result[1];
          rotated_pts[loop][2] = result[2];
        }
      }
      else {
        result[0] = rhs[0];
        result[1] = rhs[1];
        result[2] = rhs[2];
      }

      if (ext == ".fac" || ext == ".pts")
        fprintf(f, "%.2f\t%.2f\t%.2f\n", result[0], result[1], result[2]);

    }
    if (ext == ".fac" || ext == ".pts") {
      fclose(f);
      printf("%s saved to disk\n", ptsfilename);
    }

    //save .fac file
    if (ext == ".fac") {
      f = fopen(filename, "w");
      for (loop = 0; loop < geom->numelements; loop++) {
        fprintf(f, "%ld\t%ld\t%ld\n", geom->elements[loop][0] + 1,
                geom->elements[loop][1] + 1, geom->elements[loop][2] + 1);
      }
      fclose(f);
      printf("%s saved to disk\n", filename);
    }

    //save .geom/mat file
    if (ext == ".geom" || ext == ".mat") {

      //save old geom points, and restore after save
      if (transforms[i]) {
        float **oldpts = geom->points[geom->geom_index];
        geom->points[geom->geom_index] = rotated_pts;
        geomlist.push_back(geom);
        geom_oldpts.push_back(oldpts);
      }
      else {
        geomlist.push_back(geom);
        geom_oldpts.push_back(0);
      }
    }
    // save landmarks to disk (so the points rotate with the mesh rotation)
    // copy old landmark to preserve memory
    if (mesh->geom->landmarks) {
      char landmarksfile[257];
      strcpy(landmarksfile, filename);
      StripExtension(landmarksfile);
      strcat(landmarksfile, ".lmark");

      if (transforms[i]) {
        Land_Mark origlandmark = *mesh->geom->landmarks;
        Land_Mark lm;
        lm.numsegs = origlandmark.numsegs;
        lm.segs = new LandMarkSeg[lm.numsegs];

        for (loop = 0; loop < lm.numsegs; loop++) {
          int numpts = origlandmark.segs[loop].numpts;
          LandMarkSeg *seg = &lm.segs[loop];
          LandMarkSeg *origseg = &origlandmark.segs[loop];
          seg->numpts = numpts;
          seg->type = origseg->type;
          for (loop2 = 0; loop2 < 80; loop2++)  // this is the size of a segstring
            seg->segstring[loop2] = origseg->segstring[loop2];
          seg->pts = Alloc_fmatrix(numpts, 3);
          seg->rad = new float[numpts];
          for (loop2 = 0; loop2 < numpts; loop2++) {
            float temp[4];        // assign temp point, as points don't have the 4th value
            float result[4];
            temp[0] = origseg->pts[loop2][0];
            temp[1] = origseg->pts[loop2][1];
            temp[2] = origseg->pts[loop2][2];
            temp[3] = 1;
            MultMatrix16x4(product, &temp[0], &result[0]);
            seg->pts[loop2][0] = result[0];
            seg->pts[loop2][1] = result[1];
            seg->pts[loop2][2] = result[2];
            seg->rad[loop2] = origseg->rad[loop2];
          }
        }

        // write landmarks and free temp landmark copy
        WriteLandMarks(&lm, 1, landmarksfile, map3d_info.reportlevel);
        for (loop = 0; loop < lm.numsegs; loop++) {
          delete[]lm.segs[loop].rad;
          Free_fmatrix(lm.segs[loop].pts, lm.segs[loop].numpts);
        }
        delete[]lm.segs;
      }
      else {                      // don't make a copy of landmarks
        WriteLandMarks(mesh->geom->landmarks, 1, landmarksfile, map3d_info.reportlevel);
      }
    }
  }

  // finally save the geom file...
  if (ext == ".geom" || ext == ".mat") {
    if (ext == ".geom")
      WriteMap3dGeomFile(filename, geomlist);
    else
      WriteMatlabGeomFile(filename, geomlist);
    printf("%s saved to disk\n", filename);
  
    // put the geoms back to original pts
    for (unsigned i = 0; i < geomlist.size(); i++) {
      if (geom_oldpts[i]) {// we saved points in this case
        Free_fmatrix(geomlist[i]->points[geomlist[i]->geom_index], geomlist[i]->numpts);
        geomlist[i]->points[geomlist[i]->geom_index] = geom_oldpts[i];
      }
    }
  }
}

void GeomWindow::setInitialMenuChecks()
{
  // some of the Gtk functions can do infinite callbackss
  static bool menulock = false;

  if (menulock) return;

  picking.setActive(map3d_info.pickmode);
  scaling_range.setActive(map3d_info.scale_scope);
  scaling_function.setActive(map3d_info.scale_model);
  scaling_map.setActive(map3d_info.scale_mapping);

  int surface_color = 0;
  int surface_render = 0;
  int draw_mesh = 0;
  int contour_num = 2;
  int draw_contours = 1;
  bool invert = false;
  for (unsigned i = 0; i < meshes.size(); i++) {
    Mesh_Info* mesh = meshes[i];
    if (mesh->cmap == &Grayscale) surface_color = 2;
    else if (mesh->cmap == &Green2Red && surface_color == 0) surface_color = 1;
    else if (mesh->cmap == &Jet && surface_color == 0) surface_color = 3;

    if (mesh->shadingmodel > surface_render) surface_render = mesh->shadingmodel;
    if (mesh->drawmesh > draw_mesh) draw_mesh = mesh->drawmesh;
//    if (mesh->use_spacing) contour_num = 0;
    if (mesh->negcontdashed) draw_contours = 0;
    if (mesh->invert) invert = true;
  }
  menulock = true;
  surf_color.setActive(surface_color);
  surf_render.setActive(surface_render);
  mesh_render.setActive(draw_mesh);
  cont_num.setActive(contour_num);
  cont_draw_style.setActive(draw_contours);
  surf_invert.setValue(0, invert);

  int frame_step = 0;
  if (fstep == 1) frame_step = 1;
  if (fstep == 2) frame_step = 2;
  if (fstep == 4) frame_step = 3;
  if (fstep == 5) frame_step = 4;
  if (fstep == 10) frame_step = 5;
  if (fstep == 45) frame_step = 6;
  if (fstep == 90) frame_step = 7;
  frame_num.setActive(frame_step);
  menulock = false;
}

Mesh_List GeomWindow::findMeshesFromSameInput(Mesh_Info* mesh)
{
  Mesh_List newmeshes;
  for (unsigned i = 0; i < meshes.size(); i++)
    if (meshes[i]->mysurf == mesh->mysurf)
      newmeshes.push_back(meshes[i]);
  if (newmeshes.size() == 0)
    newmeshes.push_back(mesh);
  return newmeshes;
}