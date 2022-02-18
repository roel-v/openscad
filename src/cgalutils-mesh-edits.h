// Portions of this file are Copyright 2021 Google LLC, and licensed under GPL2+. See COPYING.
#pragma once

#include <iterator>
#include <list>
#include <unordered_set>
#include <CGAL/Kernel/global_functions.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/boost/graph/helpers.h>
#include <CGAL/boost/graph/properties.h>
#include <CGAL/Polygon_mesh_processing/triangulate_faces.h>
#include "grid.h"

template <typename T, typename IndicesContainer>
bool removeItemsAtSortedIndices(std::vector<T>& items, const IndicesContainer& removalIndices) {
  if (removalIndices.empty()) {
    return false;
  }
  size_t outputIndex = 0;
  auto removalIndexIt = removalIndices.begin();

  for (size_t i = 0, n = items.size(); i < n; i++) {
    if (removalIndexIt != removalIndices.end() && i == *removalIndexIt) {
      ++removalIndexIt;
      continue;
    }
    if (i != outputIndex) {
      items[outputIndex] = items[i];
    }
    outputIndex++;
  }
  items.resize(outputIndex);
  return true;
}

namespace CGALUtils {

namespace PMP = CGAL::Polygon_mesh_processing;

/*! Buffer of changes to be applied to a triangle mesh. */
template <typename TriangleMesh>
class TriangleMeshEdits
{

private:
  typedef boost::graph_traits<TriangleMesh> GT;
  typedef typename GT::face_descriptor face_descriptor;
  typedef typename GT::halfedge_descriptor halfedge_descriptor;
  typedef typename GT::edge_descriptor edge_descriptor;
  typedef typename GT::vertex_descriptor vertex_descriptor;

  std::unordered_set<face_descriptor> facesToRemove;
  std::unordered_set<vertex_descriptor> verticesToRemove;
  std::vector<std::vector<vertex_descriptor>> facesToAdd;
  std::unordered_map<vertex_descriptor, vertex_descriptor> vertexReplacements;

public:

  void removeFace(const face_descriptor& f) {
    facesToRemove.insert(f);
  }

  void removeVertex(const vertex_descriptor& v) {
    verticesToRemove.insert(v);
  }

  void addFace(const std::vector<vertex_descriptor>& vertices) {
    facesToAdd.push_back(vertices);
  }

  void replaceVertex(const vertex_descriptor& original, const vertex_descriptor& replacement) {
    vertexReplacements[original] = replacement;
  }

  bool isEmpty() { return facesToRemove.empty() && verticesToRemove.empty() && facesToAdd.empty(); }

  static bool collapsePathWithConsecutiveCollinearEdges(std::vector<vertex_descriptor>& path, const TriangleMesh& tm) {
    if (path.size() <= 3) {
      return false;
    }

    std::vector<size_t> indicesToRemove;

    const auto *p1 = &tm.point(path[0]);
    const auto *p2 = &tm.point(path[1]);
    const auto *p3 = &tm.point(path[2]);

    for (size_t i = 0, n = path.size(); i < n; i++) {
      if (CGAL::are_ordered_along_line(*p1, *p2, *p3)) {
        // p2 (at index i + 1) can be removed.
        if (indicesToRemove.empty()) {
          // Late reservation: when we start to have stuff to remove, think big and reserve the worst case.
          indicesToRemove.reserve(std::min(n - i, n - 3));
        }
        indicesToRemove.push_back(i + 1);
      }
      p1 = p2;
      p2 = p3;
      p3 = &tm.point(path[(i + 3) % n]);
    }

    if (path.size() - indicesToRemove.size() < 3) {
      return false;
    }

    return removeItemsAtSortedIndices(path, indicesToRemove);
  }

  /*! Mutating in place is tricky, to say the least, so this creates a new mesh
   * and overwrites the original to it at the end for now. */
  void apply(TriangleMesh& src)
  {
    TriangleMesh copy;

    auto edgesAdded = 0;
    for (auto& vs : facesToAdd) edgesAdded += vs.size();

    auto projectedVertexCount = src.number_of_vertices() - verticesToRemove.size();
    auto projectedHalfedgeCount = src.number_of_halfedges() + edgesAdded * 2; // This is crude
    auto projectedFaceCount = src.number_of_faces() - facesToRemove.size() + facesToAdd.size();
    copy.reserve(copy.number_of_vertices() + projectedVertexCount,
                 copy.number_of_halfedges() + projectedHalfedgeCount,
                 copy.number_of_faces() + projectedFaceCount);

    std::unordered_map<vertex_descriptor, vertex_descriptor> vertexMap;
    vertexMap.reserve(projectedVertexCount);

    auto getDestinationVertex = [&](auto srcVertex) {
        auto repIt = vertexReplacements.find(srcVertex);
        if (repIt != vertexReplacements.end()) {
          srcVertex = repIt->second;
        }
        auto it = vertexMap.find(srcVertex);
        if (it == vertexMap.end()) {
          auto v = copy.add_vertex(src.point(srcVertex));
          vertexMap[srcVertex] = v;
          return v;
        }
        return it->second;
      };

    std::vector<vertex_descriptor> polygon;

    for (auto f : src.faces()) {
      if (facesToRemove.find(f) != facesToRemove.end()) {
        continue;
      }
      polygon.clear();

      CGAL::Vertex_around_face_iterator<TriangleMesh> vit, vend;
      for (boost::tie(vit, vend) = vertices_around_face(src.halfedge(f), src); vit != vend; ++vit) {
        auto v = *vit;
        polygon.push_back(getDestinationVertex(v));
      }

      auto face = copy.add_face(polygon);
      if (!face.is_valid()) {
        std::cerr << "Failed to add face\n";
      } else {
        if (polygon.size() > 3) {
          PMP::triangulate_face(face, copy);
        }
      }
    }

    for (auto& originalPolygon : facesToAdd) {
      polygon.clear();

      for (auto v : originalPolygon) {
        polygon.push_back(getDestinationVertex(v));
      }

      auto face = copy.add_face(polygon);
      if (!face.is_valid()) {
        std::cerr << "Failed to add face\n";
      } else {
        if (polygon.size() > 3) {
          PMP::triangulate_face(face, copy);
        }
      }
    }

    src = copy;
  }
};

} // namespace CGALUtils
