#include "LocalMesh.hpp"

#include <igl/boundary_facets.h>

namespace polyfem::mesh
{
	LocalMesh::LocalMesh(
		const WildRemeshing2D &m,
		const std::vector<Tuple> &triangle_tuples,
		const bool include_global_boundary)
	{
		std::unordered_set<size_t> global_triangle_ids;

		m_triangles.resize(triangle_tuples.size(), 3);
		for (int fi = 0; fi < num_triangles(); fi++)
		{
			const Tuple &t = triangle_tuples[fi];
			global_triangle_ids.insert(t.fid(m));

			const std::array<Tuple, 3> face = m.oriented_tri_vertices(t);
			for (int i = 0; i < 3; ++i)
			{
				const size_t vi = face[i].vid(m);
				if (m_global_to_local.find(vi) == m_global_to_local.end())
					m_global_to_local[vi] = m_global_to_local.size();
				m_triangles(fi, i) = m_global_to_local[vi];
			}

			m_body_ids.push_back(m.face_attrs[t.fid(m)].body_id);
		}
		// The above puts local vertices at front
		m_num_local_vertices = num_vertices();

		for (int fi = 0; fi < num_triangles(); fi++)
		{
			const Tuple &t = triangle_tuples[fi];

			for (int ei = 0; ei < 3; ++ei)
			{
				const Tuple e = m.tuple_from_edge(t.fid(m), ei);
				if (e.switch_face(m) && global_triangle_ids.find(e.switch_face(m)->fid(m)) == global_triangle_ids.end())
				{
					m_fixed_vertices.push_back(m_global_to_local[e.vid(m)]);
					m_fixed_vertices.push_back(m_global_to_local[e.switch_vertex(m).vid(m)]);
				}
			}
		}

		// ---------------------------------------------------------------------

		if (include_global_boundary)
		{
			const std::unordered_map<int, int> prev_global_to_local = m_global_to_local;

			const std::vector<Tuple> global_boundary_edges = m.boundary_edges();
			m_boundary_edges.resize(global_boundary_edges.size(), 2);
			for (int ei = 0; ei < global_boundary_edges.size(); ei++)
			{
				const Tuple &e = global_boundary_edges[ei];
				const std::array<size_t, 2> vs = {{e.vid(m), e.switch_vertex(m).vid(m)}};

				const bool is_new_edge =
					prev_global_to_local.find(vs[0]) == prev_global_to_local.end()
					|| prev_global_to_local.find(vs[1]) == prev_global_to_local.end();

				for (int i = 0; i < 2; ++i)
				{
					if (m_global_to_local.find(vs[i]) == m_global_to_local.end())
						m_global_to_local[vs[i]] = m_global_to_local.size();
					m_boundary_edges(ei, i) = m_global_to_local[vs[i]];
					if (is_new_edge)
						m_fixed_vertices.push_back(m_boundary_edges(ei, i));
				}

				m_boundary_ids.push_back(m.edge_attrs[e.eid(m)].boundary_id);
			}
		}
		else
		{
			igl::boundary_facets(m_triangles, m_boundary_edges);
			for (int i = 0; i < m_boundary_edges.rows(); i++)
			{
				for (int j = 0; j < m_boundary_edges.cols(); j++)
				{
					m_fixed_vertices.push_back(m_boundary_edges(i, i));
				}
				// TODO:
				// m_boundary_ids.push_back(m.edge_attrs[e.eid(m)].boundary_id);
			}
		}

		remove_duplicate_fixed_vertices();

		// ---------------------------------------------------------------------

		init_vertex_attributes(m);

		init_local_to_global();
	}

	LocalMesh LocalMesh::n_ring(
		const WildRemeshing2D &m,
		const WildRemeshing2D::Tuple &center,
		const int n,
		const bool include_global_boundary)
	{
		std::vector<Tuple> triangles = m.get_one_ring_tris_for_vertex(center);
		std::unordered_set<size_t> visited_vertices{{center.vid(m)}};
		std::unordered_set<size_t> visited_faces;
		for (const auto &triangle : triangles)
			visited_faces.insert(triangle.fid(m));

		std::vector<Tuple> new_triangles = triangles;

		for (int i = 1; i < n; i++)
		{
			std::vector<Tuple> new_new_triangles;
			for (const auto &t : new_triangles)
			{
				const std::array<Tuple, 3> vs = m.oriented_tri_vertices(t);
				for (int vi = 0; vi < 3; vi++)
				{
					const Tuple &v = vs[vi];
					if (visited_vertices.find(v.vid(m)) != visited_vertices.end())
						continue;
					visited_vertices.insert(v.vid(m));

					std::vector<wmtk::TriMesh::Tuple> tmp = m.get_one_ring_tris_for_vertex(v);
					for (auto &t1 : tmp)
					{
						if (visited_faces.find(t1.fid(m)) != visited_faces.end())
							continue;
						visited_faces.insert(t1.fid(m));
						triangles.push_back(t1);
						new_new_triangles.push_back(t1);
					}
				}
			}
			new_triangles = new_new_triangles;
			if (new_triangles.empty())
				break;
		}

		return LocalMesh(m, triangles, include_global_boundary);
	}

	void LocalMesh::remove_duplicate_fixed_vertices()
	{
		std::sort(m_fixed_vertices.begin(), m_fixed_vertices.end());
		auto new_end = std::unique(m_fixed_vertices.begin(), m_fixed_vertices.end());
		m_fixed_vertices.erase(new_end, m_fixed_vertices.end());
	}

	void LocalMesh::init_local_to_global()
	{
		m_local_to_global.resize(m_global_to_local.size(), -1);
		for (const auto &[glob_vi, loc_vi] : m_global_to_local)
		{
			assert(loc_vi < m_local_to_global.size());
			m_local_to_global[loc_vi] = glob_vi;
		}
	}

	void LocalMesh::init_vertex_attributes(const WildRemeshing2D &m)
	{
		m_rest_positions.resize(num_vertices(), DIM);
		m_positions.resize(num_vertices(), DIM);
		m_prev_positions.resize(num_vertices(), DIM);
		m_prev_velocities.resize(num_vertices(), DIM);
		m_prev_accelerations.resize(num_vertices(), DIM);
		for (const auto &[glob_vi, loc_vi] : m_global_to_local)
		{
			m_rest_positions.row(loc_vi) = m.vertex_attrs[glob_vi].rest_position;
			m_positions.row(loc_vi) = m.vertex_attrs[glob_vi].position;

			assert(m.vertex_attrs[glob_vi].projection_quantities.cols() == 3);

			m_prev_positions.row(loc_vi) = m.vertex_attrs[glob_vi].prev_displacement();
			m_prev_velocities.row(loc_vi) = m.vertex_attrs[glob_vi].prev_velocity();
			m_prev_accelerations.row(loc_vi) = m.vertex_attrs[glob_vi].prev_acceleration();
		}
	}
} // namespace polyfem::mesh