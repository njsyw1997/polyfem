#include "ContactForm.hpp"
#include "BodyForm.hpp"

#include <polyfem/utils/Types.hpp>
#include <polyfem/utils/Timer.hpp>
#include <polyfem/utils/Logger.hpp>

#include <ipc/barrier/adaptive_stiffness.hpp>
#include <ipc/utils/world_bbox_diagonal_length.hpp>

namespace polyfem::solver
{
	ContactForm::ContactForm(const State &state,
							 const double dhat,
							 const bool use_adaptive_barrier_stiffness,
							 const bool is_time_dependent,
							 const ipc::BroadPhaseMethod broad_phase_method,
							 const double ccd_tolerance,
							 const int ccd_max_iterations,
							 const double acceleration_scaling,
							 const BodyForm &body_form)
		: state_(state),
		  dhat_(dhat),
		  use_adaptive_barrier_stiffness_(use_adaptive_barrier_stiffness),
		  is_time_dependent_(is_time_dependent),
		  broad_phase_method_(broad_phase_method),
		  ccd_tolerance_(ccd_tolerance),
		  ccd_max_iterations_(ccd_max_iterations),
		  body_form_(body_form),
		  acceleration_scaling_(acceleration_scaling)
	{
		assert(dhat_ > 0);
		assert(ccd_tolerance > 0);

		// use_adaptive_barrier_stiffness = !state.args["solver"]["contact"]["barrier_stiffness"].is_number();
		if (use_adaptive_barrier_stiffness_)
		{
			barrier_stiffness_ = 1;
			logger().debug("Using adaptive barrier stiffness");
		}
		else
		{
			// assert(state.args["solver"]["contact"]["barrier_stiffness"].is_number());
			assert(barrier_stiffness_ >= 0);
			// _barrier_stiffness = state.args["solver"]["contact"]["barrier_stiffness"];
			log_and_throw_error("Fixed barrier stiffness not implemented!");
			logger().debug("Using fixed barrier stiffness of {}", barrier_stiffness_);
		}

		// _broad_phase_method = state.args["solver"]["contact"]["CCD"]["broad_phase"];
		// _ccd_tolerance = state.args["solver"]["contact"]["CCD"]["tolerance"];
		// _ccd_max_iterations = state.args["solver"]["contact"]["CCD"]["max_iterations"];

		prev_distance_ = -1;
	}

	void ContactForm::init(const Eigen::VectorXd &x)
	{
		if (use_adaptive_barrier_stiffness_)
		{
			initialize_barrier_stiffness(x);
		}
	}

	Eigen::MatrixXd ContactForm::compute_displaced_surface(const Eigen::VectorXd &x) const
	{
		return state_.collision_mesh.vertices(
			state_.boundary_nodes_pos + utils::unflatten(x, state_.mesh->dimension()));
	}

	void ContactForm::initialize_barrier_stiffness(const Eigen::VectorXd &x)
	{
		const Eigen::MatrixXd displaced_surface = compute_displaced_surface(x);
		update_constraint_set(displaced_surface);

		Eigen::MatrixXd grad_energy(x.size(), 1);
		grad_energy.setZero();

		const auto &gbases = state_.geom_bases();
		state_.assembler.assemble_energy_gradient(state_.formulation(), state_.mesh->is_volume(), state_.n_bases, state_.bases, gbases, state_.ass_vals_cache, x, grad_energy);

		// TODO
		//  if (!ignore_inertia && is_time_dependent)
		grad_energy += state_.mass * x / acceleration_scaling_;

		Eigen::VectorXd body_energy(x.size());
		body_form_.first_derivative(x, body_energy);
		grad_energy += body_energy;

		Eigen::VectorXd grad_barrier = ipc::compute_barrier_potential_gradient(
			state_.collision_mesh, displaced_surface, constraint_set_, dhat_);
		grad_barrier = state_.collision_mesh.to_full_dof(grad_barrier);

		barrier_stiffness_ = ipc::initial_barrier_stiffness(
			ipc::world_bbox_diagonal_length(displaced_surface), dhat_, state_.avg_mass,
			grad_energy, grad_barrier, max_barrier_stiffness_);

		logger().debug("adaptive barrier form stiffness {}", barrier_stiffness_);
	}

	void ContactForm::update_constraint_set(const Eigen::MatrixXd &displaced_surface)
	{
		// Store the previous value used to compute the constraint set to avoid duplicate computation.
		static Eigen::MatrixXd cached_displaced_surface;
		if (cached_displaced_surface.size() == displaced_surface.size() && cached_displaced_surface == displaced_surface)
			return;

		if (use_cached_candidates_)
			ipc::construct_constraint_set(
				candidates_, state_.collision_mesh, displaced_surface, dhat_, constraint_set_);
		else
			ipc::construct_constraint_set(
				state_.collision_mesh, displaced_surface, dhat_,
				constraint_set_, /*dmin=*/0, broad_phase_method_);
		cached_displaced_surface = displaced_surface;
	}

	double ContactForm::value(const Eigen::VectorXd &x) const
	{
		return barrier_stiffness_ * ipc::compute_barrier_potential(state_.collision_mesh, compute_displaced_surface(x), constraint_set_, dhat_);
	}

	void ContactForm::first_derivative(const Eigen::VectorXd &x, Eigen::VectorXd &gradv) const
	{
		gradv = barrier_stiffness_ * ipc::compute_barrier_potential_gradient(state_.collision_mesh, compute_displaced_surface(x), constraint_set_, dhat_);
		gradv = state_.collision_mesh.to_full_dof(gradv);
	}

	void ContactForm::second_derivative(const Eigen::VectorXd &x, StiffnessMatrix &hessian)
	{
		POLYFEM_SCOPED_TIMER("\t\tbarrier hessian");
		hessian = barrier_stiffness_ * ipc::compute_barrier_potential_hessian(state_.collision_mesh, compute_displaced_surface(x), constraint_set_, dhat_, project_to_psd_);
		hessian = state_.collision_mesh.to_full_dof(hessian);
	}

	void ContactForm::solution_changed(const Eigen::VectorXd &new_x)
	{
		update_constraint_set(compute_displaced_surface(new_x));
	}

	double ContactForm::max_step_size(const Eigen::VectorXd &x0, const Eigen::VectorXd &x1) const
	{
		// Extract surface only
		const Eigen::MatrixXd V0 = compute_displaced_surface(x0);
		const Eigen::MatrixXd V1 = compute_displaced_surface(x1);

		// write_obj("s0.obj", V0, state_.collision_mesh.edges(), state_.collision_mesh.faces());
		// write_obj("s1.obj", V1, state_.collision_mesh.edges(), state_.collision_mesh.faces());

		double max_step;
		if (use_cached_candidates_ && broad_phase_method_ != ipc::BroadPhaseMethod::SWEEP_AND_TINIEST_QUEUE_GPU)
			max_step = ipc::compute_collision_free_stepsize(
				candidates_, state_.collision_mesh, V0, V1, ccd_tolerance_, ccd_max_iterations_);
		else
			max_step = ipc::compute_collision_free_stepsize(
				state_.collision_mesh, V0, V1, broad_phase_method_, ccd_tolerance_, ccd_max_iterations_);

#ifndef NDEBUG
		// This will check for static intersections as a failsafe. Not needed if we use our conservative CCD.
		Eigen::MatrixXd V_toi = (V1 - V0) * max_step + V0;

		while (ipc::has_intersections(state_.collision_mesh, V_toi))
		{
			logger().error("taking max_step results in intersections (max_step={:g})", max_step);
			max_step /= 2.0;

			const double Linf = (V_toi - V0).lpNorm<Eigen::Infinity>();
			if (max_step <= 0 || Linf == 0)
				log_and_throw_error(fmt::format("Unable to find an intersection free step size (max_step={:g} L∞={:g})", max_step, Linf));

			V_toi = (V1 - V0) * max_step + V0;
		}
#endif

		return max_step;
	}

	void ContactForm::line_search_begin(const Eigen::VectorXd &x0, const Eigen::VectorXd &x1)
	{
		ipc::construct_collision_candidates(
			state_.collision_mesh,
			compute_displaced_surface(x0),
			compute_displaced_surface(x1),
			candidates_,
			/*inflation_radius=*/dhat_ / 1.99, // divide by 1.99 instead of 2 to be conservative
			broad_phase_method_);

		use_cached_candidates_ = true;
	}

	void ContactForm::line_search_end()
	{
		candidates_.clear();
		use_cached_candidates_ = false;
	}

	void ContactForm::post_step(const int iter_num, const Eigen::VectorXd &x)
	{
		const Eigen::MatrixXd displaced_surface = compute_displaced_surface(x);

		// TODO
		//  if (state.args["output"]["advanced"]["save_nl_solve_sequence"])
		//  {
		//  	write_obj(state.resolve_output_path(fmt::format("step{:03d}.obj", iter_num)),
		//  			  displaced_surface, state_.collision_mesh.edges(), state_.collision_mesh.faces());
		//  }
		const double curr_distance = ipc::compute_minimum_distance(state_.collision_mesh, displaced_surface, constraint_set_);

		if (prev_distance_ >= 0 && use_adaptive_barrier_stiffness_)
		{
			if (is_time_dependent_)
			{
				const double prev_barrier_stiffness = barrier_stiffness_;

				barrier_stiffness_ = ipc::update_barrier_stiffness(
					prev_distance_, curr_distance, max_barrier_stiffness_,
					barrier_stiffness_, ipc::world_bbox_diagonal_length(displaced_surface));

				if (prev_barrier_stiffness != barrier_stiffness_)
				{
					polyfem::logger().debug(
						"updated barrier stiffness from {:g} to {:g}",
						prev_barrier_stiffness, barrier_stiffness_);
				}
			}
			else
			{
				initialize_barrier_stiffness(x);
			}
		}

		prev_distance_ = curr_distance;
	}

	void ContactForm::update_quantities(const double t, const Eigen::VectorXd &x)
	{
		if (use_adaptive_barrier_stiffness_)
		{
			initialize_barrier_stiffness(x);
		}
	}
} // namespace polyfem::solver
