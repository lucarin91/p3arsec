//
// FastFlow version by Daniele De Sensi (d.desensi.software@gmail.com)
//
#include "LRT/include/lrt.h"
#include "RTTL/common/RTInclude.hxx"
#include "RTTL/common/RTThread.hxx"
#include "RTTL/common/Timer.hxx"
#include "RTTL/common/RTShader.hxx"
#include "RTTL/BVH/BVH.hxx"
#include "RTTL/Mesh/Mesh.hxx"
#include "RTTL/Triangle/Triangle.hxx"
#include "RTTL/Texture/Texture.hxx"
#include "RTTL/BVH/Builder/OnDemandBuilder.hxx"

#include "LRT/FrameBuffer.hxx"
#if USE_PBOS
#include "LRT/FrameBuffer/PBOFrameBuffer.hxx"
#endif

#ifdef __wald__
// # define USE_GRID
#endif
#ifdef USE_GRID
# include "RTTL/Grid/Grid.hxx"
#endif

#ifdef ENABLE_NORNIR_NATIVE
#undef BLOCKING_MODE
#include <nornir.hpp>
#endif // ENABLE_NORNIR_NATIVE

#ifdef ENABLE_NORNIR
#include <instrumenter.hpp>
#include <stdlib.h>
#include <iostream>
#endif

#if defined(ENABLE_NORNIR) || defined(ENABLE_NORNIR_NATIVE)
std::string getParametersPath(){
    return std::string(getenv("PARSECDIR")) + std::string("/parameters.xml");
}
#endif

#ifdef FF_VERSION
#undef _INLINE
#define FF_PARFOR_PASSIVE_NOSTEALING
#include <ff/map.hpp>
#include <ff/parallel_for.hpp>
#undef _INLINE
#define _INLINE inline __attribute__((always_inline))
#endif // FF_VERSION

#ifdef ENABLE_CAF
// CAF_V1 | CAF_V2 | CAF_V3 | CAF_V4 | CAF_V4DET
#if !defined(CAF_V1) && !defined(CAF_V2) && !defined(CAF_V3) \
    && !defined(CAF_V4) && !defined(CAF_V4DET)
// #define CAF_V4
#  error "DEFINED CAF_Vx"
#endif
#include "caf/all.hpp"
#endif // ENABLE_CAF

#define NORMALIZE_PRIMARY_RAYS

#define RAY_PACKET_LAYOUT_TRIANGLE    STORE_NEAR_FAR_DISTANCE | MIN_MAX_RECIPROCAL
#define RAY_PACKET_LAYOUT_SUBDIVISION STORE_NEAR_FAR_DISTANCE | MIN_MAX_RECIPROCAL | STORE_VERTEX_NORMALS

/* -- packet of PACKET_WIDTH * PACKET_WIDTH rays -- */
#define PACKET_WIDTH 8
#define PACKET_WIDTH_SHIFT 3
#define SIMD_WIDTH 4
#define SIMD_VECTORS_PER_PACKET (PACKET_WIDTH*PACKET_WIDTH/SIMD_WIDTH)
#define SIMD_VECTORS_PER_ROW (PACKET_WIDTH/SIMD_WIDTH)
#define RAYS_PER_PACKET (PACKET_WIDTH*PACKET_WIDTH)
#define FOR_ALL_SIMD_VECTORS_IN_PACKET for (unsigned int i=0;i<SIMD_VECTORS_PER_PACKET;i++)

/* -- screen tile used for scheduling work on threads -- */
#define TILE_WIDTH (4*PACKET_WIDTH)
#define TILE_WIDTH_SHIFT 5


#define CAST_FLOAT(s,x) ((float*)&(s))[x]
#define CAST_INT(s,x)   ((int*)&(s))[x]
#define CAST_UINT(s,x)  ((unsigned int*)&(s))[x]

_ALIGN(DEFAULT_ALIGNMENT) static float coordX[RAYS_PER_PACKET] = {
  0,1,2,3,4,5,6,7,
  0,1,2,3,4,5,6,7,
  0,1,2,3,4,5,6,7,
  0,1,2,3,4,5,6,7,
  0,1,2,3,4,5,6,7,
  0,1,2,3,4,5,6,7,
  0,1,2,3,4,5,6,7,
  0,1,2,3,4,5,6,7
};

_ALIGN(DEFAULT_ALIGNMENT) static float coordY[RAYS_PER_PACKET] = {
  0,0,0,0,0,0,0,0,
  1,1,1,1,1,1,1,1,
  2,2,2,2,2,2,2,2,
  3,3,3,3,3,3,3,3,
  4,4,4,4,4,4,4,4,
  5,5,5,5,5,5,5,5,
  6,6,6,6,6,6,6,6,
  7,7,7,7,7,7,7,7
};

static const sse_f factor = _mm_set_ps1(255.0f);

using namespace RTTL;
using namespace std;


#ifdef ENABLE_CAF
using wend = caf::atom_constant<caf::atom("wend")>;
CAF_ALLOW_UNSAFE_MESSAGE_TYPE(std::function<void(size_t)>)

#ifdef CAF_V1
const std::string CAF_V = "CAF_V1";
caf::behavior pfor_worker(caf::event_based_actor *self, uint64_t i, uint64_t nw) {
  return {
    [=](const size_t& start, const size_t& end, const std::function<void(size_t)>& fun) {
      for (size_t i = start; i < end; ++i) {
        fun(i);
      }
      return wend::value;
    }
  };
}
struct map_state {
  std::vector<caf::actor> worker;
};
// caf::behavior pfor_act(caf::stateful_actor<map_state> *self, uint64_t nw) {
//   // create workers
//   self->state.worker.resize(nw);
//   for (uint32_t i = 0; i < nw; i++) {
//     caf::actor a = self->spawn<caf::lazy_init>(pfor_worker, i, nw);
//     self->state.worker[i] = a;
//   }
//   return {[=](const size_t& start, const size_t& end,
//               const size_t& grain, const std::function<void(size_t)>& fun) {
//     size_t nv = end - start;
//     size_t chunk = nv / nw;
//     size_t plus = nv % nw;
//     if (grain > 0 && grain < chunk ) {
//       chunk = grain;
//       plus = nv % grain;
//     }
    
//     auto promis = self->make_response_promise();
//     auto n_res = make_shared<size_t>(nw);
//     auto update_cb = [=](wend) mutable {
//       if (--(*n_res) == 0) {
//         promis.deliver(wend::value);
//       }
//     };

//     size_t p_start = start;
//     uint32_t iw = 0;
//     while(p_start < end) {
//         size_t p_end = p_start + chunk;
//         if (plus > 0){
//           p_end++;
//           plus--;
//         }
//         self->request(self->state.worker[iw], caf::infinite,
//                       p_start, p_end, fun).then(update_cb);
//         p_start = p_end;
//         iw = (iw + 1) % nw;
//     }
//     return promis;
//   }};
// }
caf::behavior pfor_act(caf::stateful_actor<map_state> *self, uint64_t nw) {
  // create workers
  self->state.worker.resize(nw);
  for (uint64_t i = 0; i < nw; i++) {
    caf::actor a = self->spawn(pfor_worker, i, nw);
    self->state.worker[i] = a;
  }
  return {[=](const size_t& start, const size_t& end, const std::function<void(size_t)>& fun) {
    size_t nv = end - start;
    size_t chunk = nv / nw;
    size_t plus = nv % nw;
    
    auto promis = self->make_response_promise();
    auto n_res = make_shared<uint64_t>(nw);
    auto update_cb = [=](wend) mutable {
      if (--(*n_res) == 0) {
        promis.deliver(wend::value);
      }
    };

    size_t p_start = start;
    uint32_t iw = 0;
    while(p_start < end) {
        size_t p_end = p_start + chunk;
        if (plus > 0){
          p_end++;
          plus--;
        }
        self->request(self->state.worker[iw], caf::infinite,
                      p_start, p_end, fun).then(update_cb);
        p_start = p_end;
        iw = (iw + 1) % nw;
    }
    return promis;
  }};
}
#else
#ifdef CAF_V2
const std::string CAF_V = "CAF_V2";
caf::behavior pfor_worker(caf::event_based_actor *self, uint64_t i, uint64_t nw) {
  return {
    [=](const size_t& start, const size_t& end, const std::function<void(size_t)>& fun) {
      for (size_t i = start; i < end; ++i) {
        fun(i);
      }
      return wend::value;
    }
  };
}
caf::behavior pfor_act(caf::event_based_actor *self) {
  return {[=](const size_t& start, const size_t& end,
              const size_t& grain, const std::function<void(size_t)>& fun) {
    size_t nv = end - start;
    size_t nw = nv / grain;
    size_t plus = nv % grain;
    
    auto promis = self->make_response_promise();
    auto n_res = make_shared<size_t>(nw);
    auto update_cb = [=](wend) mutable {
      if (--(*n_res) == 0) {
        promis.deliver(wend::value);
      }
    };

    size_t p_start = start;
    for (auto i=0; i<nw; i++){
      size_t p_end = p_start + grain;
      if (plus > 0) {
        p_end++;
        plus--;
      }
      caf::actor worker = self->spawn(pfor_worker, i, nw);
      self->request(worker, caf::infinite, p_start, p_end, fun)
            .then(update_cb);
      p_start = p_end;
    }
    return promis;
  }};
}
#else
#ifdef CAF_V3
const std::string CAF_V = "CAF_V3";
caf::behavior pfor_worker(caf::event_based_actor *self, uint64_t i) {
  return {
    [=](const size_t& start, const size_t& end, const std::function<void(size_t)>& fun) {
      for (size_t i = start; i < end; ++i) {
        fun(i);
      }
      return wend::value;
    }
  };
}
struct map_state {
  std::vector<caf::actor> worker;
};
caf::behavior pfor_act(caf::stateful_actor<map_state> *self, uint64_t nw) {
  // create workers
  self->state.worker.resize(nw);
  for (uint64_t i = 0; i < nw; i++) {
    caf::actor a = self->spawn(pfor_worker, i);
    self->state.worker[i] = a;
  }
  // caf::aout(self) << "DEBUG " << "nw=" << nw << " "
  //                 << "worker.size=" << self->state.worker.size() << std::endl;
  return {[=](const size_t& start, const size_t& end,
              const size_t& grain, const std::function<void(size_t)>& fun) {
    size_t nv = end - start;
    size_t w_spawn = nv / grain;
    size_t plus = nv % grain;
    size_t nw = self->state.worker.size();
    // caf::aout(self) << "DEBUG "
    //                 << "nv=" << nv << " "
    //                 << "grain=" << grain << " "
    //                 << "w_spawn=" << w_spawn << " "
    //                 << "nw=" << nw << std::endl;
    for (auto i = nw; i < w_spawn; i++) {
      caf::actor a = self->spawn(pfor_worker, i);
      self->state.worker.push_back(a);
      // caf::aout(self) << "+";
    }
    if (nw < w_spawn) {
      caf::aout(self) << std::endl << "spawned " << w_spawn << " worker" << "("
                    << "nv=" << nv << ","
                    << "grain=" << grain << ")" << std::endl;
    }
    nw = self->state.worker.size();
    // caf::aout(self) << "DEBUG " << "nw=" << nw << std::endl;

    auto promis = self->make_response_promise();
    auto n_res = make_shared<size_t>(nw);
    auto update_cb = [=](wend) mutable {
      if (--(*n_res) == 0) {
        promis.deliver(wend::value);
      }
    };

    size_t p_start = start;
    uint32_t iw = 0;
    while(p_start < end) {
        size_t p_end = p_start + grain;
        if (plus > 0){
          p_end++;
          plus--;
        }
        self->request(self->state.worker[iw], caf::infinite,
                      p_start, p_end, fun).then(update_cb);
        // caf::aout(self) << "DEBUG "
        //                 << "worker=" << iw << " "
        //                 << "p_start=" << p_start << " "
        //                 << "p_end=" << p_end << " "
        //                 << "len=" << p_end - p_start << std::endl;
        p_start = p_end;
        iw = (iw + 1) % nw;
    }
    return promis;
  }};
}
#else
#if defined(CAF_V4) || defined(CAF_V4DET)
#ifdef CAF_V4DET
const std::string CAF_V = "CAF_V4DET";
#else
const std::string CAF_V = "CAF_V4";
#endif
using wget = caf::atom_constant<caf::atom("wget")>;
atomic<size_t> *atomic_i;
caf::behavior pfor_worker(caf::event_based_actor *self, uint64_t iw,
                          caf::actor emitter) {
  return {[=](const size_t &start, const size_t &end,
              const std::function<void(size_t)> &fun) {
            // caf::aout(self) << "DEBUG "
            //                 << "->worker " << iw << " "
            //                 << "get " << start << " - " << end << std::endl;
            self->send(emitter, wget::value);
            for (auto i = start; i < end; ++i) {
              fun(i);
            }
            return wend::value;
          },
          [=](const size_t &size, const std::function<void(size_t)> &fun) {
            size_t i;
            while ((i = atomic_i->fetch_add(1)) < size) {
              // caf::aout(self) << "DEBUG "
              //                 << "->worker " << iw << " "
              //                 << "get " << i << std::endl;
              fun(i);
            }
            return wend::value;
          }};
}
void pfor_act(caf::blocking_actor *self, uint64_t nw) {
  // create workers
  vector<caf::actor> worker(nw);
  for (auto i = 0; i < nw; i++) {
#ifdef CAF_V4DET
    worker[i] = self->spawn<caf::detached>(pfor_worker, i, caf::actor_cast<caf::actor>(self));
#else
    worker[i] = self->spawn(pfor_worker, i, caf::actor_cast<caf::actor>(self));
#endif
  }
  bool running = true;
  self->receive_while(running)(
      [=](const size_t &start, const size_t &end, const size_t &grain,
          const std::function<void(size_t)> &fun) {
        auto sender = caf::actor_cast<caf::actor>(self->current_sender());
        size_t nv = end - start;

        if (grain == 1) {
          // use the atomic version
          atomic_i = new atomic<size_t>(0);

          for (auto w : worker) {
            self->send(w, nv, fun);
          }
          size_t i{0};
          self->receive_for(i, worker.size())([=](wend) {});
          free(atomic_i);
        } else {
          // use the generic with message version
          size_t chunk = nv / nw;
          size_t plus = nv % nw;
          // if grain is specified use it
          if (grain > 0 && grain < chunk) {
            chunk = grain;
            plus = nv % grain;
          }

          size_t p_start = start;
          size_t n_res = 0;
          auto send_chunk = [&](const caf::actor& to) {
            size_t p_end = p_start + chunk;
            if (plus > 0) {
              p_end++;
              plus--;
            }
            self->send(to, p_start, p_end, fun);
            n_res++;
            p_start = p_end;
          };

          for (auto w : worker) {
            if (p_start < end) {
              send_chunk(w);
            } else {
              break;
            }
          }

          bool receive = true;
          self->receive_while(receive)(
              [&](wget) {
                if (p_start < end) {
                  send_chunk(
                      caf::actor_cast<caf::actor>(self->current_sender()));
                }
              },
              [&](wend) {
                --n_res;
                if (p_start >= end && n_res == 0)
                  receive = false;
              });
        }
        self->response(wend::value);
      },
      [&](caf::exit_msg &em) {
        if (em.reason) {
          self->fail_state(std::move(em.reason));
          running = false;
        }
      });
}
#endif // CAF_V4
#endif // CAF_V3
#endif // CAF_V2
#endif // CAF_V1

#endif // ENABLE_CAF

class Camera {
public:
  RTVec3f m_cameraOrigin;
  RTVec3f m_cameraDirection;
  RTVec3f m_cameraUp;
  float m_cameraViewAngle;
  float m_cameraAspectRatio;
  float m_cameraDistance;


  inline float getCameraAspect() { return m_cameraAspectRatio; };
  inline void setCameraAspect(float aspect) { m_cameraAspectRatio = aspect; };

  inline void setCamera(const RTVec3f &origin,
			const RTVec3f &direction,
			const RTVec3f &up,
			const float angle,
			const float aspect)
  {
    m_cameraOrigin = origin;
    m_cameraDirection = direction.normalize();
    m_cameraUp = up.normalize();
    m_cameraViewAngle = angle;
    m_cameraAspectRatio = aspect;
    m_cameraDistance = 0.5f / tanf(angle * M_PI / 180.0f / 2.0f);
  }

};

class Context;

class Context : public MultiThreadedTaskQueue
{
protected:

  /* data shared by all threads */
  struct SharedThreadData {
    /* camera data in SSE friendly layout */
    RTVec_t<3,sse_f> origin;
    RTVec_t<3,sse_f> up;
    RTVec_t<3,sse_f> imagePlaneOrigin;
    RTVec_t<3,sse_f> xAxis;
    RTVec_t<3,sse_f> yAxis;
    RTVec_t<3,sse_f> zAxis;
    int resX;
    int resY;
    int maxTiles;
    LRT::FrameBuffer *frameBuffer;
  };

  /* scene */

  int m_geometryMode;
  PolygonalBaseMesh *m_mesh;
  BVH *m_bvh;
  vector< RTMaterial, Align<RTMaterial> > m_material;
  vector< RTTextureObject_RGBA_UCHAR*, Align<RTTextureObject_RGBA_UCHAR*> > m_texture;

  /* threads */
  int m_threads;
  bool m_threadsCreated;

  // need to be aligned, therefore made static
  static SharedThreadData m_threadData;
  static AtomicCounter m_tileCounter;

#ifdef FF_VERSION
	ff::ParallelFor* pf;
#endif
#ifdef ENABLE_CAF
  std::shared_ptr<caf::actor_system> system;
  std::shared_ptr<caf::scoped_actor> self;
  caf::actor pf;
  uint32_t caf_conf_wpt;
  uint32_t caf_conf_grain;
  uint32_t caf_conf_act;
#endif // ENABLE_CAF
#ifdef ENABLE_NORNIR_NATIVE
  nornir::ParallelFor* pf;
#endif

  // Nornir: Create instrumenter
#ifdef ENABLE_NORNIR
    nornir::Instrumenter* instr;
#endif

  /* textures */

  _INLINE void initSharedThreadData(Camera *camera,
				    const int resX,
				    const int resY,
				    LRT::FrameBuffer *frameBuffer)
  {
    const float left = -camera->m_cameraAspectRatio * 0.5f;
    const float top  = 0.5f;

    m_threadData.origin[0] = convert(camera->m_cameraOrigin[0]);
    m_threadData.origin[1] = convert(camera->m_cameraOrigin[1]);
    m_threadData.origin[2] = convert(camera->m_cameraOrigin[2]);
    m_threadData.yAxis[0]  = convert(camera->m_cameraDirection[0]);
    m_threadData.yAxis[1]  = convert(camera->m_cameraDirection[1]);
    m_threadData.yAxis[2]  = convert(camera->m_cameraDirection[2]);
    m_threadData.yAxis.normalize();
    m_threadData.up[0]     = convert(camera->m_cameraUp[0]);
    m_threadData.up[1]     = convert(camera->m_cameraUp[1]);
    m_threadData.up[2]     = convert(camera->m_cameraUp[2]);
    m_threadData.xAxis     = m_threadData.yAxis^m_threadData.up;
    m_threadData.xAxis.normalize();
    m_threadData.zAxis     = m_threadData.yAxis^m_threadData.xAxis;
    m_threadData.zAxis.normalize();

    m_threadData.imagePlaneOrigin = m_threadData.yAxis * convert(camera->m_cameraDistance) + convert(left) * m_threadData.xAxis - convert(top) * m_threadData.zAxis;
    m_threadData.xAxis     = m_threadData.xAxis * camera->m_cameraAspectRatio / resX;
    m_threadData.zAxis     = m_threadData.zAxis / resY;
    m_threadData.resX      = resX;
    m_threadData.resY      = resY;
    m_threadData.maxTiles  = (resX >> PACKET_WIDTH_SHIFT)*(resY >> PACKET_WIDTH_SHIFT);
    m_threadData.frameBuffer = frameBuffer;
  }

  virtual int task(int jobID, int threadID);

  template <class MESH, const int LAYOUT>
  _INLINE void renderTile(LRT::FrameBuffer *frameBuffer,
			  const int startX,const int startY,
			  const int resX,const int resY);

public:
  enum {
    MINIRT_POLYGONAL_GEOMETRY,
    MINIRT_SUBDIVISION_SURFACE_GEOMETRY
  };


  Context(){
    m_bvh = NULL;
    m_mesh = NULL;
    m_threads = 1;
    m_threadsCreated = false;
    m_geometryMode = MINIRT_POLYGONAL_GEOMETRY;
    Context::m_tileCounter.reset();
#ifdef FF_VERSION
	pf = NULL;
#endif
#ifdef ENABLE_CAF
    std::cout << "CAF_VERSION=" << CAF_VERSION << " " << CAF_V << std::endl;
    caf_conf_wpt = 1;
    if(const char* env_wpt = std::getenv("CAF_CONF_WPT")){
      caf_conf_wpt = atoi(env_wpt);
    }
    caf_conf_act = 0;
    if(const char* env_act = std::getenv("CAF_CONF_ACT")){
      caf_conf_act = atoi(env_act);
    }
#if defined(CAF_V2) || defined(CAF_V3) || defined(CAF_V4) || defined(CAF_V4DET)
    caf_conf_grain = 1;
    if(const char* env_grain = std::getenv("CAF_CONF_GRAIN")){
      caf_conf_grain = atoi(env_grain);
    }
#endif  // CAF_V2 / CAF_V3
#endif // ENABLE_CAF
#ifdef ENABLE_NORNIR
    instr = new nornir::Instrumenter(getParametersPath());
#endif
  }

  ~Context(){
#ifdef ENABLE_NORNIR
    instr->terminate();
    std::cout << "riff.time|" << instr->getExecutionTime() << std::endl;
    std::cout << "riff.iterations|" << instr->getTotalTasks() << std::endl;
    delete instr;
#endif 
  }

  /* ------------------------------------ */
  /* -------------- Mini API ------------ */
  /* ------------------------------------ */

  void init(const int mode = MINIRT_POLYGONAL_GEOMETRY);
  void setRenderThreads(const int threads);
  void clear();

  void addVertices(const RTVec3f *const v,const RTVec2f *const txt,const int vertices);
  void addTriangleMesh(const RTVec3i *const t,const int triangles, const int *const shaderID = NULL);
  void addQuadMesh(const RTVec4i *const t,const int quads, const int *const shaderID = NULL);
  void addMaterials(const RTMaterial *const mat, const int materials);
  void addTexture(const int width, const int height, void *data, const int format);

  void finalize();
  void buildSpatialIndexStructure();

  void renderFrame(Camera *camera,
		   LRT::FrameBuffer *frameBuffer,
		   const int resX,const int resY);

  _INLINE int numPrimitives() const
  {
    return m_mesh->numPrimitives();
  }

  _INLINE int numVertices() const
  {
    return m_mesh->numVertices();
  }

  _INLINE int numMaterials() const
  {
    return m_material.size();
  }

  _INLINE RTBoxSSE getSceneAABB() const
  {
    return m_mesh->getAABB();
  }
};

_ALIGN(DEFAULT_ALIGNMENT) Context::SharedThreadData Context::m_threadData;
_ALIGN(DEFAULT_ALIGNMENT) AtomicCounter Context::m_tileCounter;


/*! get four pixels in sse-float-format, converts those to RGB-uchar */
_INLINE sse_i convert_fourPixels_to_fourRBGAuchars(const sse_f& red,
						   const sse_f& green,
						   const sse_f& blue)
{
  sse_i r  = _mm_cvtps_epi32(red   * factor);
  sse_i g  = _mm_cvtps_epi32(green * factor);
  sse_i b  = _mm_cvtps_epi32(blue  * factor);
  sse_i fc = _mm_or_si128(_mm_slli_epi32(r, 16), _mm_or_si128(b, _mm_slli_epi32(g, 8)));
  return fc;
}

/* ----------------------------------------------------------------------------------------------------------------- */
/* -- small shaders mostly for debugging and testing, will be removed as soon as the shading compiler is working  -- */
/* ----------------------------------------------------------------------------------------------------------------- */




/* moved constants outside the function as otherwise the compiler does not treat them as constants */

static const sse_i moduloX = convert<sse_i>(11);
static const sse_i moduloY = convert<sse_i>(13);
static const sse_i moduloZ = convert<sse_i>(17);
static const sse_f scaleX  = convert<sse_f>(1.0f / 11);
static const sse_f scaleY  = convert<sse_f>(1.0f / 13);
static const sse_f scaleZ  = convert<sse_f>(1.0f / 17);
static const sse_i bias    = convert<sse_i>(12);

template <int N, int LAYOUT, int MULTIPLE_ORIGINS, int SHADOW_RAYS, class Mesh>
_INLINE void Shade_RandomID(RayPacket<N, LAYOUT, MULTIPLE_ORIGINS, SHADOW_RAYS> &packet,
			    const Mesh &mesh,
			    const RTMaterial *const mat,
			    RTTextureObject_RGBA_UCHAR **texture,
			    sse_i *const dest)
{
  FOR_ALL_SIMD_VECTORS_IN_PACKET
    {
      const sse_i t = packet.id(i) + bias;
      const sse_f colorX = convert(t & moduloX) * scaleX;
      const sse_f colorY = convert(t & moduloY) * scaleY;
      const sse_f colorZ = convert(t & moduloZ) * scaleZ;
      dest[i] = convert_fourPixels_to_fourRBGAuchars(colorX,colorY,colorZ);
    }
}

template <int N, int LAYOUT, int MULTIPLE_ORIGINS, int SHADOW_RAYS, class Mesh>
_INLINE void Shade_PrimitiveID(RayPacket<N, LAYOUT, MULTIPLE_ORIGINS, SHADOW_RAYS> &packet,
			       const Mesh &mesh,
			       const RTMaterial *const mat,
			       RTTextureObject_RGBA_UCHAR **texture,
			       sse_i *const dest)
{
  FOR_ALL_SIMD_VECTORS_IN_PACKET
    {
      const sse_f t = convert(packet.id(i));
      dest[i] = convert_fourPixels_to_fourRBGAuchars(t,t,t);
    }
}

template <int N, int LAYOUT, int MULTIPLE_ORIGINS, int SHADOW_RAYS, class Mesh>
_INLINE void Shade_ShaderID(RayPacket<N, LAYOUT, MULTIPLE_ORIGINS, SHADOW_RAYS> &packet,
			    const Mesh &mesh,
			    const RTMaterial *const mat,
			    RTTextureObject_RGBA_UCHAR **texture,
			    sse_i *const dest)
{
  FOR_ALL_SIMD_VECTORS_IN_PACKET
    {
      const sse_i t = packet.shaderID(i) + bias;
      const sse_f colorX = convert(t & moduloX) * scaleX;
      const sse_f colorY = convert(t & moduloY) * scaleY;
      const sse_f colorZ = convert(t & moduloZ) * scaleZ;
      dest[i] = convert_fourPixels_to_fourRBGAuchars(colorX,colorY,colorZ);
    }
}

template <int N, int LAYOUT, int MULTIPLE_ORIGINS, int SHADOW_RAYS, class Mesh>
_INLINE void Shade_Diffuse(RayPacket<N, LAYOUT, MULTIPLE_ORIGINS, SHADOW_RAYS> &packet,
			   const Mesh &mesh,
			   const RTMaterial *const mat,
			   RTTextureObject_RGBA_UCHAR **texture,
			   sse_i *const dest)
{
  RTVec_t<3, sse_f> diffuse;
  FOR_ALL_SIMD_VECTORS_IN_PACKET
    {
      RTMaterial::getDiffuse(packet.shaderID(i),mat,diffuse);
      //DBG_PRINT(diffuse);
      dest[i] = convert_fourPixels_to_fourRBGAuchars(diffuse[0],diffuse[1],diffuse[2]);
    }
}

template <int N, int LAYOUT, int MULTIPLE_ORIGINS, int SHADOW_RAYS, class Mesh>
_INLINE void Shade_Normal(RayPacket<N, LAYOUT, MULTIPLE_ORIGINS, SHADOW_RAYS> &packet,
			  const Mesh &mesh,
			  const RTMaterial *const mat,
			  RTTextureObject_RGBA_UCHAR **texture,
			  sse_i *const dest)
{
  RTVec_t<3, sse_f> normal;
  FOR_ALL_SIMD_VECTORS_IN_PACKET
    {
      mesh.getGeometryNormal<N, LAYOUT, MULTIPLE_ORIGINS, SHADOW_RAYS,false>(packet,i,normal);
      dest[i] = convert_fourPixels_to_fourRBGAuchars(normal[0],normal[1],normal[2]);
    }
}

template <int N, int LAYOUT, int MULTIPLE_ORIGINS, int SHADOW_RAYS, class Mesh>
_INLINE void Shade_EyeLight(RayPacket<N, LAYOUT, MULTIPLE_ORIGINS, SHADOW_RAYS> &packet,
			    const Mesh &mesh,
			    const RTMaterial *const mat,
			    RTTextureObject_RGBA_UCHAR **texture,                
			    sse_i *const dest)
{
  RTVec_t<3, sse_f> normal;
  const sse_f fixedColor = convert<sse_f>(0.6f);
  const sse_f ambient = convert<sse_f>(0.2f);

  FOR_ALL_SIMD_VECTORS_IN_PACKET
    {
      mesh.template getGeometryNormal<N, LAYOUT, MULTIPLE_ORIGINS, SHADOW_RAYS,true>(packet,i,normal);
      // needs normalized ray directions 
      const sse_f dot = abs(normal[0] * packet.directionX(i) + normal[1] * packet.directionY(i) + normal[2] * packet.directionZ(i));
      const sse_f color = ambient + fixedColor * dot;
      dest[i] = convert_fourPixels_to_fourRBGAuchars(color,color,color);
    }
}

template <int N, int LAYOUT, int MULTIPLE_ORIGINS, int SHADOW_RAYS, class Mesh>
_INLINE void Shade_TxtCoord(RayPacket<N, LAYOUT, MULTIPLE_ORIGINS, SHADOW_RAYS> &packet,
			    const Mesh &mesh,
			    const RTMaterial *const mat,
			    RTTextureObject_RGBA_UCHAR **texture,
			    sse_i *const dest)
{
  RTVec_t<2, sse_f> txt;
  RTVec_t<4, sse_f> texel;
  FOR_ALL_SIMD_VECTORS_IN_PACKET
    {
      mesh.getTextureCoordinate<N, LAYOUT, MULTIPLE_ORIGINS, SHADOW_RAYS,false>(packet,i,txt);
      //DBG_PRINT(mat.m_textureId);
      //texture[mat[0].m_textureId]->getTexel(txt[0],txt[1],texel);
      dest[i] = convert_fourPixels_to_fourRBGAuchars(txt[0],txt[1],convert<sse_f>(1) - txt[0] - txt[1]);
      //dest[i] = convert_fourPixels_to_fourRBGAuchars(texel[0],texel[1],texel[2]);
    }
}

template <int N, int LAYOUT, int MULTIPLE_ORIGINS, int SHADOW_RAYS, class Mesh>
_INLINE void Shade_Texture(RayPacket<N, LAYOUT, MULTIPLE_ORIGINS, SHADOW_RAYS> &packet,
			   const Mesh &mesh,
			   const RTMaterial *const mat,
			   RTTextureObject_RGBA_UCHAR **texture,
			   sse_i *const dest)
{
  RTVec_t<2, sse_f> txt;
  RTVec_t<4, sse_f> texel;
  const sse_i zero = convert<sse_i>(0);
  const sse_i noHit = convert<sse_i>(-1);
  FOR_ALL_SIMD_VECTORS_IN_PACKET
    {
      texel[0] = _mm_setzero_ps();
      texel[1] = _mm_setzero_ps();
      texel[2] = _mm_setzero_ps();

      const sse_f noHitMask = _mm_castsi128_ps(_mm_cmpgt_epi32(packet.id(i), noHit));
      //if (__builtin_expect(_mm_movemask_ps(noHitMask) == 0x0,0)) continue;
      const sse_i shaderID = max(packet.shaderID(i),zero); // -1 not allowed

      mesh.getTextureCoordinate<N, LAYOUT, MULTIPLE_ORIGINS, SHADOW_RAYS,false>(packet,i,txt);

      const int txtId0 = mat[CAST_INT(shaderID,0)].m_textureId;
      const int txtId1 = mat[CAST_INT(shaderID,1)].m_textureId;
      const int txtId2 = mat[CAST_INT(shaderID,2)].m_textureId;
      const int txtId3 = mat[CAST_INT(shaderID,3)].m_textureId;

      if (txtId0 != -1) texture[txtId0]->getTexel<0>(txt[0],txt[1],texel);
      if (txtId1 != -1) texture[txtId1]->getTexel<1>(txt[0],txt[1],texel);
      if (txtId2 != -1) texture[txtId2]->getTexel<2>(txt[0],txt[1],texel);
      if (txtId3 != -1) texture[txtId3]->getTexel<3>(txt[0],txt[1],texel);

      texel[0] &= noHitMask;
      texel[1] &= noHitMask;
      texel[2] &= noHitMask;

#if 0
      DBG_PRINT(packet.id(i));
      DBG_PRINT(packet.shaderID(i));

      DBG_PRINT(txtId0);
      DBG_PRINT(txtId1);
      DBG_PRINT(txtId2);
      DBG_PRINT(txtId3);
      DBG_PRINT(txt[0]);
      DBG_PRINT(txt[1]);

      DBG_PRINT(texel[0]);
      DBG_PRINT(texel[1]);
      DBG_PRINT(texel[2]);
      DBG_PRINT(texel[3]);
      exit(0);
#endif

      dest[i] = convert_fourPixels_to_fourRBGAuchars(texel[0],texel[1],texel[2]);
    }
}

/* --------------------------------------------------------------------------------------------------- */
/* --------------------------------------------------------------------------------------------------- */
/* --------------------------------------------------------------------------------------------------- */

void Context::init(const int mode)
{
#ifndef RT_EMULATE_SSE
  const int oldMXCSR = _mm_getcsr();
  const int newMXCSR = oldMXCSR | (_MM_FLUSH_ZERO_ON | _MM_MASK_MASK); // | _MM_ROUND_TOWARD_ZERO
  _mm_setcsr(newMXCSR);
#endif

  m_geometryMode = mode;

  assert(m_mesh == NULL);
  
  switch(m_geometryMode)
    {
    case MINIRT_POLYGONAL_GEOMETRY:
      m_mesh = new (aligned_malloc<StandardTriangleMesh>(1)) StandardTriangleMesh; // ugly I know
      break;
    case MINIRT_SUBDIVISION_SURFACE_GEOMETRY:
      m_mesh = new (aligned_malloc<DirectedEdgeMesh>(1)) DirectedEdgeMesh; // ugly I know
      break;
    default:
      FATAL("Context: unkown geometry mode");
    }      
}

void Context::setRenderThreads(const int nthreads)
{
  m_threads = nthreads;
}

void Context::clear()
{
  m_mesh->clear();
}

void Context::addVertices(const RTVec3f *const v,const RTVec2f *const txt,const int vertices)
{
  m_mesh->addVertices((float*)v,(float*)txt,vertices,RT_VERTEX_3F);
}

void Context::addTriangleMesh(const RTVec3i* const t,const int triangles, const int *const shaderID)
{
  m_mesh->addPrimitives((int *)t,triangles,RT_TRIANGLE,shaderID);
}

void Context::addQuadMesh(const RTVec4i* const t,const int quads, const int *const shaderID)
{
  m_mesh->addPrimitives((int *)t,quads,RT_QUAD,shaderID);
}

void Context::addMaterials(const RTMaterial *const mat, const int materials)
{
  m_material.reserve(materials);
  for (int i=0;i<materials;i++)
    m_material.push_back(mat[i]);
}

void Context::addTexture(const int width, const int height, void *data, const int format)
{
  assert(format == RT_TEXTURE_FORMAT_RGB_UCHAR);
  RTTextureObject_RGBA_UCHAR* txt = new RTTextureObject_RGBA_UCHAR(width,height);
  /* need here a more sophisticated conversion framework */
#if 1
  RTTextureObject_RGBA_UCHAR::Texel *dest = txt->getTexelPtr();
  unsigned char *source = (unsigned char*)data;
  for (int i=0;i<width*height;i++)
    {
      dest[i][0] = source[0];
      dest[i][1] = source[1];
      dest[i][2] = source[2];
      dest[i][3] = 0;
      source += 3;
    }
#endif
  m_texture.push_back(txt);
}


void Context::finalize()
{
  m_mesh->finalize();
  if (m_material.size() == 0)
    {
      cout << "No materials -> create dummy material" << endl;
      RTMaterial mat;
      mat.m_diffuse = RTVec3f(0.8f,0.8f,0.8f);
      m_material.push_back(mat);
    }
}

void Context::buildSpatialIndexStructure()
{
  assert(m_mesh->numPrimitives());
  assert(m_mesh->numVertices());
  assert(m_bvh == NULL);
  
  const int numPrimitives = m_mesh->numPrimitives();
  AABB *box = aligned_malloc<AABB>(numPrimitives);
  m_mesh->storePrimitiveAABBs(box,numPrimitives);

  //for (int i=0;i<mesh->numPrimitives();i++)
  //  box[i] = static_cast<AABB>(mesh->getAABB(i));

  m_bvh = new AABBListBVH(box,numPrimitives);

  Timer timer;
  timer.start();

  m_bvh->build(m_mesh->getAABB(),m_mesh->getCentroidAABB());

  const float t = timer.stop();
  cout << "build time " << t << endl;

#ifdef USE_GRID
  {
    Timer timer;
    timer.start();

    AABBPrimList(mesh->getAABB());
    int IDs = mesh->triangles;
    int *ID = new int[IDs];
    for (int i=0;i<IDs;i++) ID[i] = i;
    RecursiveGrid *grid = new RecursiveGrid(primList,ID,0,IDs);

    const float t = timer.stop();
    cout << "grid build time " << t << endl;
  }
#endif

  free_align(box);

}


#if defined(FF_VERSION) || defined(ENABLE_CAF)
int Context::task(int jobID, int threadId){;}

#else 
int Context::task(int jobID, int threadId)
{
  const int tilesPerRow = m_threadData.resX >> TILE_WIDTH_SHIFT;
  while(1)
    {
      int index = Context::m_tileCounter.inc();
      if (index >= m_threadData.maxTiles) break;

      /* todo: get rid of '/' and '%' */

      int sx = (index % tilesPerRow)*TILE_WIDTH;
      int sy = (index / tilesPerRow)*TILE_WIDTH;
      int ex = min(sx+TILE_WIDTH,m_threadData.resX);
      int ey = min(sy+TILE_WIDTH,m_threadData.resY);

      if (m_geometryMode == MINIRT_POLYGONAL_GEOMETRY)
	renderTile<StandardTriangleMesh,RAY_PACKET_LAYOUT_TRIANGLE>(m_threadData.frameBuffer,sx,sy,ex,ey);
      else if (m_geometryMode == MINIRT_SUBDIVISION_SURFACE_GEOMETRY)
	renderTile<DirectedEdgeMesh,RAY_PACKET_LAYOUT_SUBDIVISION>(m_threadData.frameBuffer,sx,sy,ex,ey);
      else
	FATAL("unknown mesh type");
    }

  return THREAD_RUNNING;
}
#endif



/*! render a frame, write pixels to framebuffer. in its original
  version, the framebuffer was specified manually by a pointer; I
  changed that to wrap frame buffer handling in its own class. _that_
  many virtual functions should be allowed, I guess ;-) */
void Context::renderFrame(Camera *camera,
			 LRT::FrameBuffer *frameBuffer,
                         const int resX,const int resY)
{
    assert(camera);
  if (m_threadsCreated == false)
    {
      if (m_threads > 1)
	{
	  cout << "-> starting " << m_threads << " threads..." << flush;    
#ifdef FF_VERSION
      // FastFlow: ParallelFor
      pf = new ff::ParallelFor(m_threads, false, true);
      pf->disableScheduler();
#else
#ifdef ENABLE_CAF
    caf::actor_system_config cfg;
#ifdef DETACHED_WORKER
    cfg.set("scheduler.max-threads", 1);
#else
    cfg.set("scheduler.max-threads", m_threads);
#endif
    system = make_shared<caf::actor_system>(cfg);
    self = make_shared<caf::scoped_actor>(*system);
#if defined(CAF_V1) || defined(CAF_V3) || defined(CAF_V4) || defined(CAF_V4DET)
    uint64_t nw = caf_conf_act == 0 ? m_threads * caf_conf_wpt : caf_conf_act;
    std::cout << "N. thread: " << m_threads << " "
              << "N. actor: "  << nw << " "<< std::flush;
    pf = system->spawn(pfor_act, nw);
#endif // CAF_V1 | CAF_V3 | CAF_V4 | CAF_V4DET
#ifdef CAF_V2
    pf = system->spawn<caf::detached>(pfor_act);
#endif// CAF_V2
#if defined(CAF_V2) || defined(CAF_V3) || defined(CAF_V4) || defined(CAF_V4DET)
    std::cout << "(grain=" << caf_conf_grain << ") " << std::flush;
#endif
#else
#ifdef ENABLE_NORNIR_NATIVE 
      // Nornir: ParallelFor
      pf = new nornir::ParallelFor(m_threads, new nornir::Parameters(getParametersPath()));
#else
    // Pthreads: Create threads
	  createThreads(m_threads);
#endif // ENABLE_NORNIR_NATIVE
#endif // ENABLE_CAF
#endif //FF_VERSION
	  cout << "done" << endl << flush;
	}
      m_threadsCreated = true;
    }

// std::cout << "DEBUG: elements " << m_threadData.maxTiles << std::endl;

#ifdef ENABLE_NORNIR
#ifdef DEMO_BRIGHT17
  static long long int framenum = 0;
  if(framenum > 10000){
      instr->begin();
  }
#else
  instr->begin();
#endif // DEMO_BRIGHT17
#endif // ENABLE_NORNIR
  frameBuffer->startNewFrame();
  initSharedThreadData(camera,resX,resY,frameBuffer);

  BVH_STAT_COLLECTOR(BVHStatCollector::global.reset());
  if (m_threads>1)
    {
#ifdef FF_VERSION
        // Parallel for
        int index;
        const int tilesPerRow = m_threadData.resX >> TILE_WIDTH_SHIFT;
        pf->parallel_for(0, m_threadData.maxTiles, 1, 1, [&](const int index) 
        {
                /* todo: get rid of '/' and '%' */
                int sx = (index % tilesPerRow)*TILE_WIDTH;
                int sy = (index / tilesPerRow)*TILE_WIDTH;
                int ex = min(sx+TILE_WIDTH,m_threadData.resX);
                int ey = min(sy+TILE_WIDTH,m_threadData.resY);
                if (m_geometryMode == MINIRT_POLYGONAL_GEOMETRY)
                    renderTile<StandardTriangleMesh,RAY_PACKET_LAYOUT_TRIANGLE>(m_threadData.frameBuffer,sx,sy,ex,ey);
                else if (m_geometryMode == MINIRT_SUBDIVISION_SURFACE_GEOMETRY)
                    renderTile<DirectedEdgeMesh,RAY_PACKET_LAYOUT_SUBDIVISION>(m_threadData.frameBuffer,sx,sy,ex,ey);
                else
                    FATAL("unknown mesh type");
         }
         , m_threads
         );
#else
#ifdef ENABLE_CAF
        // Parallel for
        const int tilesPerRow = m_threadData.resX >> TILE_WIDTH_SHIFT;
        static bool size_print = false;
        if (!size_print) {
          std::cout << "N. of elements " << m_threadData.maxTiles << std::endl;
          size_print = true;
        }
        std::function<void(size_t)> fun = [&](size_t index) {
            /* todo: get rid of '/' and '%' */
            int sx = (index % tilesPerRow)*TILE_WIDTH;
            int sy = (index / tilesPerRow)*TILE_WIDTH;
            int ex = min(sx+TILE_WIDTH,m_threadData.resX);
            int ey = min(sy+TILE_WIDTH,m_threadData.resY);
            if (m_geometryMode == MINIRT_POLYGONAL_GEOMETRY)
                renderTile<StandardTriangleMesh,RAY_PACKET_LAYOUT_TRIANGLE>(m_threadData.frameBuffer,sx,sy,ex,ey);
            else if (m_geometryMode == MINIRT_SUBDIVISION_SURFACE_GEOMETRY)
                renderTile<DirectedEdgeMesh,RAY_PACKET_LAYOUT_SUBDIVISION>(m_threadData.frameBuffer,sx,sy,ex,ey);
            else
                FATAL("unknown mesh type");
          };
#ifdef CAF_V1
        auto promis = (*self)->request(pf, caf::infinite, (size_t) 0, (size_t) m_threadData.maxTiles, fun);
#else
        auto promis = (*self)->request(pf, caf::infinite, (size_t) 0, (size_t) m_threadData.maxTiles,
                                       (size_t) caf_conf_grain, fun);
#endif // CAF_V1
        promis.receive(
          [&](wend) {
            // caf::aout(*self) << "DEBUG " << "end pf computation" << endl;
          },
          [&](caf::error &_) { caf::aout(*self) << "error_" << _ << endl; }
        );
#else
#ifdef ENABLE_NORNIR_NATIVE
        // Parallel for
        int index;
        const int tilesPerRow = m_threadData.resX >> TILE_WIDTH_SHIFT;
        pf->parallel_for(0, m_threadData.maxTiles, 1, 1, 
          [&](const long long int index, const uint thid) 
        {
                /* todo: get rid of '/' and '%' */
                int sx = (index % tilesPerRow)*TILE_WIDTH;
                int sy = (index / tilesPerRow)*TILE_WIDTH;
                int ex = min(sx+TILE_WIDTH,m_threadData.resX);
                int ey = min(sy+TILE_WIDTH,m_threadData.resY);
                if (m_geometryMode == MINIRT_POLYGONAL_GEOMETRY)
                    renderTile<StandardTriangleMesh,RAY_PACKET_LAYOUT_TRIANGLE>(m_threadData.frameBuffer,sx,sy,ex,ey);
                else if (m_geometryMode == MINIRT_SUBDIVISION_SURFACE_GEOMETRY)
                    renderTile<DirectedEdgeMesh,RAY_PACKET_LAYOUT_SUBDIVISION>(m_threadData.frameBuffer,sx,sy,ex,ey);
                else
                    FATAL("unknown mesh type");
         });
#else
      Context::m_tileCounter.reset();
      startThreads();
      waitForAllThreads();
#endif // ENABLE_NORNIR_NATIVE
#endif // ENABLE_CAF
#endif // FF_VERSION
    }
  else
    if (m_geometryMode == MINIRT_POLYGONAL_GEOMETRY)
      renderTile<StandardTriangleMesh,RAY_PACKET_LAYOUT_TRIANGLE>(frameBuffer,0,0,resX,resY);
    else if (m_geometryMode == MINIRT_SUBDIVISION_SURFACE_GEOMETRY)
      renderTile<DirectedEdgeMesh,RAY_PACKET_LAYOUT_SUBDIVISION>(frameBuffer,0,0,resX,resY);
    else
      FATAL("unknown mesh type");
    
  BVH_STAT_COLLECTOR(BVHStatCollector::global.print());
  frameBuffer->doneWithFrame();
#ifdef ENABLE_NORNIR
#ifdef DEMO_BRIGHT17
  if(framenum > 10000){
      instr->end();
  }else{
      ++framenum;
  }
#else
  instr->end();
#endif
#endif
}

#define SHADE( SHADERNAME ) Shade_##SHADERNAME <SIMD_VECTORS_PER_PACKET, LAYOUT, MULTIPLE_ORIGINS, SHADOW_RAYS, MESH>(packet,mesh,mat,texture,rgb32)

template <class MESH, const int LAYOUT>
void Context::renderTile(LRT::FrameBuffer *frameBuffer,
			const int startX, 
			const int startY,
			const int endX,
			const int endY)
{
  const int MULTIPLE_ORIGINS = 0;
  const int SHADOW_RAYS = 0;
  RayPacket<SIMD_VECTORS_PER_PACKET, LAYOUT, MULTIPLE_ORIGINS, SHADOW_RAYS> packet;

  _ALIGN(DEFAULT_ALIGNMENT) sse_i rgb32[SIMD_VECTORS_PER_PACKET];

  const MESH &mesh = *dynamic_cast<MESH*>(m_mesh);
  const RTMaterial *const mat = m_material.size() ? &*m_material.begin() : NULL;
  RTTextureObject_RGBA_UCHAR **texture = m_texture.size() ?  &*m_texture.begin() : NULL;

  
  for (int y=startY; y+PACKET_WIDTH<=endY; y+=PACKET_WIDTH)
    for (int x=startX; x+PACKET_WIDTH<=endX; x+=PACKET_WIDTH)
      {
	/* init all rays within packet */
	const sse_f sx = _mm_set_ps1((float)x);
	const sse_f sy = _mm_set_ps1((float)y);
	const sse_f delta = _mm_set_ps1(PACKET_WIDTH-1);
	FOR_ALL_SIMD_VECTORS_IN_PACKET
	  {
	    const sse_f dx = _mm_add_ps(sx,_mm_load_ps(&coordX[i*SIMD_WIDTH]));
	    const sse_f dy = _mm_add_ps(sy,_mm_load_ps(&coordY[i*SIMD_WIDTH]));
	    packet.directionX(i) = _mm_add_ps(_mm_add_ps(_mm_mul_ps(dx,m_threadData.xAxis[0]),
							 _mm_mul_ps(dy,m_threadData.zAxis[0])),
					      m_threadData.imagePlaneOrigin[0]);
	    packet.directionY(i) = _mm_add_ps(_mm_add_ps(_mm_mul_ps(dx,m_threadData.xAxis[1]),
							 _mm_mul_ps(dy,m_threadData.zAxis[1])),
					      m_threadData.imagePlaneOrigin[1]);
	    packet.directionZ(i) = _mm_add_ps(_mm_add_ps(_mm_mul_ps(dx,m_threadData.xAxis[2]),
							 _mm_mul_ps(dy,m_threadData.zAxis[2])),
					      m_threadData.imagePlaneOrigin[2]);
#if defined(NORMALIZE_PRIMARY_RAYS)
	    const sse_f invLength = rsqrt(packet.directionX(i) * packet.directionX(i) + packet.directionY(i) * packet.directionY(i) + packet.directionZ(i) * packet.directionZ(i));
	    packet.directionX(i) *= invLength;
	    packet.directionY(i) *= invLength;
	    packet.directionZ(i) *= invLength;
#endif                
	    packet.originX(i) = m_threadData.origin[0];
	    packet.originY(i) = m_threadData.origin[1];
	    packet.originZ(i) = m_threadData.origin[2];
	  }
	packet.computeReciprocalDirectionsAndInitMinMax();
	packet.reset();
	TraverseBVH<SIMD_VECTORS_PER_PACKET, LAYOUT, MULTIPLE_ORIGINS, SHADOW_RAYS, MESH>(packet,m_bvh->node,m_bvh->item,mesh);

 	//SHADE(RandomID);
 	SHADE(EyeLight);

	frameBuffer->writeBlock(x,y,PACKET_WIDTH,PACKET_WIDTH,rgb32);
      }
}



LRTContext lrtCreateContext()
{
  Context *c = new Context;
  c->init(Context::MINIRT_POLYGONAL_GEOMETRY);
  return c;
}

LRTvoid lrtDestroyContext(LRTContext context)
{
  assert(context);
  delete (Context*) context;
}

LRTContext lrtCreateCamera()
{
  Camera *c = new Camera;
  return c;
}

LRTvoid lrtDestroyCamera(LRTCamera camera)
{
  assert(camera);
  delete camera;
}

LRTvoid lrtSetRenderThreads(LRTContext context, LRTuint nthreads)
{
  assert(context);
  assert(nthreads>=1);
  ((Context*)context)->setRenderThreads(nthreads);
}



LRTvoid lrtLookAt(LRTCamera camera, 
		  RTfloat eyeX, RTfloat eyeY, RTfloat eyeZ, 
		  RTfloat centerX, RTfloat centerY, RTfloat centerZ, 
		  RTfloat upX, RTfloat upY, RTfloat upZ,
		  RTfloat angle,
		  RTfloat aspect)
{
  // actually, I don't think the camera is part of the context -- you
  // might well imagine rendering the same 'scene' (i.e., context,
  // with different cameras for differnt frame buffers. but let's not
  // care about that right now

  vec3f eye(eyeX,eyeY,eyeZ);
  vec3f dir = vec3f(centerX,centerY,centerZ)-eye;
  vec3f up(upX,upY,upZ);  

  ((Camera*)camera)->setCamera(eye,dir,up,angle,aspect);
}

using LRT::FrameBuffer;


/*

  Oh my -- this is one of the greatest and ugliest hacks i've ever made.

  right now, i'm using carstens minirt code from his frontend to do the
  rendering. to do so, i go through the scene graph, and -- if i do so
  for the first time -- push the geometry into his mesh class just as a
  parser would have done ... this SUCKS

  Comment by C. Bienia:
  I separated the initialization code from the rendering code to isolate
  the ROI from everything else. The initialization code is now in the
  new function `lrtBuildContext', which has to be called before the
  actual rendering function `lrtRenderFrame'. I also moved the thread
  creation from the `renderFrame' function here.

  Sorry for destroying this masterpiece of hacking...
*/
static int initialized = false;

LRTvoid lrtBuildContext(LRTContext _context)
{

  Context *context = (Context*)_context;
  
  //make sure lrtBuildContext hasn't been called yet
  assert(!initialized);

  World *w = World::getDefaultWorld();
  assert(w);

  // OK, assume we know we have only one object right now ..... aaaargh
  assert(w->rootNode.size() == 1);
  RootNode *root = w->rootNode[0];
      
  // OK, let's further assume there's only one node in that tree, and that it's a mesh ..... uhhhh, how ugly .....
  cout << "num nodes in scene graph " << root->getNumChildren() << endl;
  assert(root->getNumChildren() == 1);
  ISG::BaseMesh *mesh = dynamic_cast<ISG::BaseMesh *>(root->getChild(0));
  assert(mesh);
      
  // And since we do such ugly things, anyway, let's assume our
  // mesh has vertices of type RT_FLOAT3, everything else is not
  // implemented, yet ...
  DataArray *vertexArray = mesh->coord;
  assert(vertexArray);
  assert(vertexArray->m_ptr != NULL);
  assert(vertexArray->type == RT_COORDS);
  if (vertexArray->format != RT_FLOAT3)
    FATAL("Only support a single mesh with RT_FLOAT3 vertices right now .... ");
  cout << "adding " << vertexArray->units << " vertices" << endl;
  context->addVertices((vec3f*)vertexArray->m_ptr,NULL,vertexArray->units);

  // Finally, do the same with the triangle array .. .assume it's there, and it's vec3i's ...
  DataArray *triangleArray = mesh->index;
  assert(triangleArray);
  assert(triangleArray->m_ptr != NULL);
  assert(triangleArray->type == RT_INDICES);
  if (triangleArray->format != RT_INT3)
    FATAL("Only support a single mesh with RT_INT3 indices right now .... ");

  cout << "adding " << triangleArray->units << " triangles" << endl;
  context->addTriangleMesh((vec3i*)triangleArray->m_ptr,triangleArray->units,NULL);
      
  cout << "finalizing geometry" << endl;
  context->finalize();
  cout << "building index" << endl;
  context->buildSpatialIndexStructure();
  cout << "done" << endl;

  RTBoxSSE sceneAABB = context->getSceneAABB();
  PRINT(sceneAABB);
      
  initialized = true;
}


LRTvoid lrtRenderFrame(LRTFrameBufferHandle _fb,
                       LRTContext _context, 
                       LRTCamera _camera
                       )
{
  Context *context = (Context*)_context;

  assert(_fb != NULL);
  FrameBuffer *frameBuffer = (FrameBuffer*)_fb;

  //make sure lrtBuildContext has been called
  assert(initialized);

  //   cout << "rendering in res " << flush << frameBuffer->res << endl;
  context->renderFrame((Camera*)_camera,
		       frameBuffer,
		       frameBuffer->res.x,
		       frameBuffer->res.y);
}

