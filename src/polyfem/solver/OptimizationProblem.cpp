#include "OptimizationProblem.hpp"
#include <polyfem/utils/StringUtils.hpp>

#include <igl/writeOBJ.h>
#include <igl/writeMESH.h>
#include <igl/write_triangle_mesh.h>

namespace polyfem
{
	namespace
	{
		void print_centers(const std::vector<Eigen::VectorXd> &centers)
		{
			std::cout << "[";
			for (int c = 0; c < centers.size(); c++)
			{
				const auto &center = centers[c];
				std::cout << "[";
				for (int d = 0; d < center.size(); d++)
				{
					std::cout << center(d);
					if (d < center.size() - 1)
						std::cout << ", ";
				}
				if (c < centers.size() - 1)
					std::cout << "],";
				else
					std::cout << "]";
			}
			std::cout << "]\n";
		}

		void print_markers(const Eigen::MatrixXd &centers, const std::vector<bool> &active_mask)
		{
			std::cout << "[";
			for (int c = 0; c < centers.rows(); c++)
			{
				if (!active_mask[c])
					continue;
				std::cout << "[";
				for (int d = 0; d < centers.cols(); d++)
				{
					std::cout << centers(c, d);
					if (d < centers.cols() - 1)
						std::cout << ", ";
				}
				if (c < centers.rows() - 1)
					std::cout << "],";
				else
					std::cout << "]";
			}
			std::cout << "]\n";
		}
	} // namespace

	OptimizationProblem::OptimizationProblem(State &state_, const std::shared_ptr<CompositeFunctional> j_) : state(state_)
	{
		j = j_;
		dim = state.mesh->dimension();
		actual_dim = state.problem->is_scalar() ? 1 : dim;

		cur_grad.resize(0);
		cur_val = std::nan("");

		opt_nonlinear_params = state.args["optimization"]["solver"]["nonlinear"];
		opt_output_params = state.args["optimization"]["output"];
		opt_params = state.args["optimization"];

		save_freq = opt_output_params["save_frequency"];
		max_change = opt_nonlinear_params["max_change"];
	}

	void OptimizationProblem::solve_pde(const TVector &x)
	{
		if (!state.problem->is_time_dependent() && opt_nonlinear_params["better_initial_guess"])
		{
			if (optimization_name == "shape")
			{
				if (x_at_ls_begin.size() == x.size())
				{
					if (sol_at_ls_begin.size() == x.size())
						state.pre_sol = sol_at_ls_begin + x_at_ls_begin - x;
					else if (sol_at_ls_begin.size() == state.n_bases)
						state.pre_sol = sol_at_ls_begin + state.down_sampling_mat.transpose() * (x_at_ls_begin - x);
				}
			}
			else if (sol_at_ls_begin.size() > 0)
			{
				state.pre_sol = sol_at_ls_begin;
			}
		}

		// control forward solve log level
		const int cur_log = state.current_log_level;
		state.set_log_level(static_cast<spdlog::level::level_enum>(opt_output_params["solve_log_level"]));

		auto output_dir = state.output_dir;
		if (state.problem->is_time_dependent() && save_iter < iter)
		{
			save_iter++;
			if (save_iter % save_freq == 0)
			{
				state.output_dir = "iter_" + std::to_string(iter);
				if (std::filesystem::exists(state.output_dir))
					std::filesystem::remove_all(state.output_dir);
				std::filesystem::create_directories(state.output_dir);
				logger().info("Save time sequence to {} ...", state.output_dir);
			}
		}

		state.assemble_rhs();
		state.assemble_stiffness_mat();
		state.solve_problem();

		if (utils::StringUtils::startswith(j->get_functional_name(), "Center"))
		{
			CenterTrajectoryFunctional f;
			f.set_interested_ids(j->get_interested_body_ids(), j->get_interested_boundary_ids());
			std::vector<Eigen::VectorXd> barycenters;
			f.get_barycenter_series(state, barycenters);
			print_centers(barycenters);
		}
		else if (j->get_functional_name() == "NodeTrajectory")
		{
			const auto &f = *dynamic_cast<NodeTrajectoryFunctional *>(j.get());
			Eigen::MatrixXd V;
			Eigen::MatrixXi F;
			state.get_vf(V, F, false);
			V.block(0, 0, V.rows(), state.mesh->dimension()) += utils::unflatten(state.sol, state.mesh->dimension());
			print_markers(V, f.get_active_vertex_mask());
		}

		state.output_dir = output_dir;
		state.set_log_level(static_cast<spdlog::level::level_enum>(cur_log));

		logger().info("Linear Solves: {}, Non-linear Solves: {}", state.n_linear_solves, state.n_nonlinear_solves);
	}

	void OptimizationProblem::solution_changed(const TVector &newX)
	{
		if (cur_x.size() == newX.size() && cur_x == newX)
			return;

		if (solution_changed_pre(newX))
			solve_pde(newX);

		solution_changed_post(newX);
	}

	void OptimizationProblem::line_search_begin(const TVector &x0, const TVector &x1)
	{
		descent_direction = x1 - x0;

		// debug
		if (opt_nonlinear_params["debug_fd"].get<bool>())
		{
			double t = 1e-6;
			TVector new_x = x0 + descent_direction * t;

			solution_changed(new_x);
			double J2 = value(new_x);

			solution_changed(x0);
			double J1 = value(x0);
			TVector gradv;
			gradient(x0, gradv);

			logger().debug("step size: {}, finite difference: {}, derivative: {}", t, (J2 - J1) / t, gradv.dot(descent_direction));
		}
	}

	void OptimizationProblem::save_to_file(const TVector &x0)
	{
		logger().info("Iter {} Save Freq {}", iter, save_freq);
		if (iter % save_freq == 0)
		{
			logger().debug("Save to file {} ...", state.resolve_output_path(fmt::format("opt_{:d}.vtu", iter)));
			state.save_vtu(state.resolve_output_path(fmt::format("opt_{:d}.vtu", iter)), 0.);

			if (!state.mesh->is_volume())
				state.mesh->save(state.resolve_output_path(fmt::format("opt_{:d}.obj", iter)));
			else
			{
				Eigen::MatrixXd V;
				Eigen::MatrixXi F;
				state.get_vf(V, F);
				igl::writeMESH(state.resolve_output_path(fmt::format("opt_{:d}.mesh", iter)), V, F, Eigen::MatrixXi());
			}
		}
	}
} // namespace polyfem