/*
  Copyright 2013, 2015 SINTEF ICT, Applied Mathematics.
  Copyright 2014, 2015 Dr. Blatt - HPC-Simulation-Software & Services
  Copyright 2014, 2015 Statoil ASA.
  Copyright 2015 NTNU
  Copyright 2015 IRIS AS

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef OPM_BLACKOILPOLYMERMODEL_IMPL_HEADER_INCLUDED
#define OPM_BLACKOILPOLYMERMODEL_IMPL_HEADER_INCLUDED

#include <opm/polymer/fullyimplicit/BlackoilPolymerModel.hpp>

#include <opm/autodiff/AutoDiffBlock.hpp>
#include <opm/autodiff/AutoDiffHelpers.hpp>
#include <opm/autodiff/GridHelpers.hpp>
#include <opm/autodiff/BlackoilPropsAdInterface.hpp>
#include <opm/autodiff/GeoProps.hpp>
#include <opm/autodiff/WellDensitySegmented.hpp>

#include <opm/core/grid.h>
#include <opm/core/linalg/LinearSolverInterface.hpp>
#include <opm/core/linalg/ParallelIstlInformation.hpp>
#include <opm/core/props/rock/RockCompressibility.hpp>
#include <opm/core/utility/ErrorMacros.hpp>
#include <opm/core/utility/Exceptions.hpp>
#include <opm/core/utility/Units.hpp>
#include <opm/core/well_controls.h>
#include <opm/core/utility/parameters/ParameterGroup.hpp>

#include <cassert>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <limits>

namespace Opm {



    namespace detail {

        template <class PU>
        int polymerPos(const PU& pu)
        {
            const int maxnp = Opm::BlackoilPhases::MaxNumPhases;
            int pos = 0;
            for (int phase = 0; phase < maxnp; ++phase) {
                if (pu.phase_used[phase]) {
                    pos++;
                }
            }

            return pos;
        }

    } // namespace detail



    template <class Grid>
    BlackoilPolymerModel<Grid>::BlackoilPolymerModel(const typename Base::ModelParameters&   param,
                                                     const Grid&                             grid,
                                                     const BlackoilPropsAdInterface&         fluid,
                                                     const DerivedGeology&                   geo,
                                                     const RockCompressibility*              rock_comp_props,
                                                     const PolymerPropsAd&                   polymer_props_ad,
                                                     const Wells*                            wells,
                                                     const NewtonIterationBlackoilInterface& linsolver,
                                                     const bool                              has_disgas,
                                                     const bool                              has_vapoil,
                                                     const bool                              has_polymer,
                                                     const bool                              has_plyshlog,
                                                     const std::vector<double>&              wells_rep_radius,
                                                     const std::vector<double>&              wells_perf_length,
                                                     const bool                              terminal_output)
        : Base(param, grid, fluid, geo, rock_comp_props, wells, linsolver,
               has_disgas, has_vapoil, terminal_output),
          polymer_props_ad_(polymer_props_ad),
          has_polymer_(has_polymer),
          has_plyshlog_(has_plyshlog),
          poly_pos_(detail::polymerPos(fluid.phaseUsage())),
          wells_rep_radius_(wells_rep_radius),
          wells_perf_length_(wells_perf_length)
    {
        if (has_polymer_) {
            if (!active_[Water]) {
                OPM_THROW(std::logic_error, "Polymer must solved in water!\n");
            }
            // If deck has polymer, residual_ should contain polymer equation.
            rq_.resize(fluid_.numPhases() + 1);
            residual_.material_balance_eq.resize(fluid_.numPhases() + 1, ADB::null());
            assert(poly_pos_ == fluid_.numPhases());
        }
    }



    template <class Grid>
    void
    BlackoilPolymerModel<Grid>::
    prepareStep(const double dt,
                ReservoirState& reservoir_state,
                WellState& well_state)
    {
        Base::prepareStep(dt, reservoir_state, well_state);
        // Initial max concentration of this time step from PolymerBlackoilState.
        cmax_ = Eigen::Map<const V>(reservoir_state.maxconcentration().data(), Opm::AutoDiffGrid::numCells(grid_));
    }




    template <class Grid>
    void
    BlackoilPolymerModel<Grid>::
    afterStep(const double /* dt */,
              ReservoirState& reservoir_state,
              WellState& /* well_state */)
    {
        computeCmax(reservoir_state);
    }





    template <class Grid>
    void
    BlackoilPolymerModel<Grid>::makeConstantState(SolutionState& state) const
    {
        Base::makeConstantState(state);
        state.concentration = ADB::constant(state.concentration.value());
    }





    template <class Grid>
    std::vector<V>
    BlackoilPolymerModel<Grid>::variableStateInitials(const ReservoirState& x,
                                                      const WellState& xw) const
    {
        std::vector<V> vars0 = Base::variableStateInitials(x, xw);
        assert(int(vars0.size()) == fluid_.numPhases() + 2);

        // Initial polymer concentration.
        if (has_polymer_) {
            assert (not x.concentration().empty());
            const int nc = x.concentration().size();
            const V c = Eigen::Map<const V>(&x.concentration()[0], nc);
            // Concentration belongs after other reservoir vars but before well vars.
            auto concentration_pos = vars0.begin() + fluid_.numPhases();
            assert(concentration_pos == vars0.end() - 2);
            vars0.insert(concentration_pos, c);
        }
        return vars0;
    }





    template <class Grid>
    std::vector<int>
    BlackoilPolymerModel<Grid>::variableStateIndices() const
    {
        std::vector<int> ind = Base::variableStateIndices();
        assert(ind.size() == 5);
        if (has_polymer_) {
            ind.resize(6);
            // Concentration belongs after other reservoir vars but before well vars.
            ind[Concentration] = fluid_.numPhases();
            // Concentration is pushing back the well vars.
            ++ind[Qs];
            ++ind[Bhp];
        }
        return ind;
    }




    template <class Grid>
    typename BlackoilPolymerModel<Grid>::SolutionState
    BlackoilPolymerModel<Grid>::variableStateExtractVars(const ReservoirState& x,
                                                         const std::vector<int>& indices,
                                                         std::vector<ADB>& vars) const
    {
        SolutionState state = Base::variableStateExtractVars(x, indices, vars);
        if (has_polymer_) {
            state.concentration = std::move(vars[indices[Concentration]]);
        }
        return state;
    }





    template <class Grid>
    void
    BlackoilPolymerModel<Grid>::computeAccum(const SolutionState& state,
                                             const int            aix  )
    {
        Base::computeAccum(state, aix);

        // Compute accumulation of polymer equation only if needed.
        if (has_polymer_) {
            const ADB& press = state.pressure;
            const std::vector<ADB>& sat = state.saturation;
            const ADB& c = state.concentration;
            const ADB pv_mult = poroMult(press); // also computed in Base::computeAccum, could be optimized.
            const Opm::PhaseUsage& pu = fluid_.phaseUsage();
            // compute polymer properties.
            const ADB cmax = ADB::constant(cmax_, state.concentration.blockPattern());
            const ADB ads  = polymer_props_ad_.adsorption(state.concentration, cmax);
            const double rho_rock = polymer_props_ad_.rockDensity();
            const V phi = Eigen::Map<const V>(&fluid_.porosity()[0], AutoDiffGrid::numCells(grid_));
            const double dead_pore_vol = polymer_props_ad_.deadPoreVol();
            // Compute polymer accumulation term.
            rq_[poly_pos_].accum[aix] = pv_mult * rq_[pu.phase_pos[Water]].b * sat[pu.phase_pos[Water]] * c * (1. - dead_pore_vol) 
                                        + pv_mult * rho_rock * (1. - phi) / phi * ads;
        }
 
    }






    template <class Grid>
    void BlackoilPolymerModel<Grid>::computeCmax(ReservoirState& state)
    {
        const int nc = AutoDiffGrid::numCells(grid_);
        V tmp = V::Zero(nc);
        for (int i = 0; i < nc; ++i) {
            tmp[i] = std::max(state.maxconcentration()[i], state.concentration()[i]);
        }
        std::copy(&tmp[0], &tmp[0] + nc, state.maxconcentration().begin());
    }





    template <class Grid>
    void
    BlackoilPolymerModel<Grid>::
    assembleMassBalanceEq(const SolutionState& state)
    {
        // Base::assembleMassBalanceEq(state);

        // Compute b_p and the accumulation term b_p*s_p for each phase,
        // except gas. For gas, we compute b_g*s_g + Rs*b_o*s_o.
        // These quantities are stored in rq_[phase].accum[1].
        // The corresponding accumulation terms from the start of
        // the timestep (b^0_p*s^0_p etc.) were already computed
        // on the initial call to assemble() and stored in rq_[phase].accum[0].
        computeAccum(state, 1);

        // Set up the common parts of the mass balance equations
        // for each active phase.
        const V transi = subset(geo_.transmissibility(), ops_.internal_faces);
        const std::vector<ADB> kr = computeRelPerm(state);


        if (has_plyshlog_) {
            std::vector<double> water_vel;
            std::vector<double> visc_mult;

            computeWaterShearVelocityFaces(transi, kr, state.canonical_phase_pressures, state, water_vel, visc_mult);
            if(!computeShearMultLog(water_vel, visc_mult, shear_mult_faces_)) {
                // std::cerr << " failed in calculating the shear-multiplier " << std::endl;
                OPM_THROW(std::runtime_error, " failed in calculating the shear-multiplier. ");
            }
        }

        for (int phaseIdx = 0; phaseIdx < fluid_.numPhases(); ++phaseIdx) {
            computeMassFlux(phaseIdx, transi, kr[canph_[phaseIdx]], state.canonical_phase_pressures[canph_[phaseIdx]], state);

            residual_.material_balance_eq[ phaseIdx ] =
                pvdt_ * (rq_[phaseIdx].accum[1] - rq_[phaseIdx].accum[0])
                + ops_.div*rq_[phaseIdx].mflux;
        }

        // -------- Extra (optional) rs and rv contributions to the mass balance equations --------

        // Add the extra (flux) terms to the mass balance equations
        // From gas dissolved in the oil phase (rs) and oil vaporized in the gas phase (rv)
        // The extra terms in the accumulation part of the equation are already handled.
        if (active_[ Oil ] && active_[ Gas ]) {
            const int po = fluid_.phaseUsage().phase_pos[ Oil ];
            const int pg = fluid_.phaseUsage().phase_pos[ Gas ];

            const UpwindSelector<double> upwindOil(grid_, ops_,
                                                rq_[po].dh.value());
            const ADB rs_face = upwindOil.select(state.rs);

            const UpwindSelector<double> upwindGas(grid_, ops_,
                                                rq_[pg].dh.value());
            const ADB rv_face = upwindGas.select(state.rv);

            residual_.material_balance_eq[ pg ] += ops_.div * (rs_face * rq_[po].mflux);
            residual_.material_balance_eq[ po ] += ops_.div * (rv_face * rq_[pg].mflux);

            // OPM_AD_DUMP(residual_.material_balance_eq[ Gas ]);

        }

        // Add polymer equation.
        if (has_polymer_) {
            residual_.material_balance_eq[poly_pos_] = pvdt_ * (rq_[poly_pos_].accum[1] - rq_[poly_pos_].accum[0])
                                               + ops_.div*rq_[poly_pos_].mflux;
        }
    }





    template <class Grid>
    void BlackoilPolymerModel<Grid>::extraAddWellEq(const SolutionState& state,
                                                    const WellState& xw,
                                                    const std::vector<ADB>& cq_ps,
                                                    const std::vector<ADB>& cmix_s,
                                                    const ADB& cqt_is,
                                                    const std::vector<int>& well_cells)
    {
        // Add well contributions to polymer mass balance equation
        if (has_polymer_) {
            const ADB mc = computeMc(state);
            const int nc = xw.polymerInflow().size();
            const V polyin = Eigen::Map<const V>(xw.polymerInflow().data(), nc);
            const V poly_in_perf = subset(polyin, well_cells);
            const V poly_mc_perf = subset(mc, well_cells).value();
            const PhaseUsage& pu = fluid_.phaseUsage();
            const ADB cq_s_poly = cq_ps[pu.phase_pos[Water]] * poly_mc_perf
                + cmix_s[pu.phase_pos[Water]] * cqt_is * poly_in_perf;
            residual_.material_balance_eq[poly_pos_] -= superset(cq_s_poly, well_cells, nc);
        }
    }






    template <class Grid>
    void BlackoilPolymerModel<Grid>::updateState(const V& dx,
                                                 ReservoirState& reservoir_state,
                                                 WellState& well_state)
    {
        if (has_polymer_) {
            // Extract concentration change.
            const int np = fluid_.numPhases();
            const int nc = Opm::AutoDiffGrid::numCells(grid_);
            const V zero = V::Zero(nc);
            const int concentration_start = nc * np;
            const V dc = subset(dx, Span(nc, 1, concentration_start));

            // Create new dx with the dc part deleted.
            V modified_dx = V::Zero(dx.size() - nc);
            modified_dx.head(concentration_start) = dx.head(concentration_start);
            const int tail_len = dx.size() - concentration_start - nc;
            modified_dx.tail(tail_len) = dx.tail(tail_len);

            // Call base version.
            Base::updateState(modified_dx, reservoir_state, well_state);

            // Update concentration.
            const V c_old = Eigen::Map<const V>(&reservoir_state.concentration()[0], nc, 1);
            const V c = (c_old - dc).max(zero);
            std::copy(&c[0], &c[0] + nc, reservoir_state.concentration().begin());
        } else {
            // Just forward call to base version.
            Base::updateState(dx, reservoir_state, well_state);
        }
    }





    template <class Grid>
    void
    BlackoilPolymerModel<Grid>::computeMassFlux(const int               actph ,
                                                const V&                transi,
                                                const ADB&              kr    ,
                                                const ADB&              phasePressure,
                                                const SolutionState&    state)
    {
        Base::computeMassFlux(actph, transi, kr, phasePressure, state);

        // Polymer treatment.
        const int canonicalPhaseIdx = canph_[ actph ];
        if (canonicalPhaseIdx == Water) {
            if (has_polymer_) {
                const std::vector<PhasePresence>& cond = phaseCondition();
                const ADB tr_mult = transMult(state.pressure);
                const ADB mu = fluidViscosity(canonicalPhaseIdx, phasePressure, state.temperature, state.rs, state.rv, cond, cells_);
                const ADB cmax = ADB::constant(cmax_, state.concentration.blockPattern());
                const ADB mc = computeMc(state);
                const ADB krw_eff = polymer_props_ad_.effectiveRelPerm(state.concentration, cmax, kr);
                const ADB inv_wat_eff_visc = polymer_props_ad_.effectiveInvWaterVisc(state.concentration, mu.value().data());
                // Reduce mobility of water phase by relperm reduction and effective viscosity increase.
                rq_[actph].mob = tr_mult * krw_eff * inv_wat_eff_visc;
                // Compute polymer mobility.
                rq_[poly_pos_].mob = tr_mult * mc * krw_eff * inv_wat_eff_visc;
                rq_[poly_pos_].b = rq_[actph].b;
                rq_[poly_pos_].dh = rq_[actph].dh;
                UpwindSelector<double> upwind(grid_, ops_, rq_[poly_pos_].dh.value());
                // Compute polymer flux.
                rq_[poly_pos_].mflux = upwind.select(rq_[poly_pos_].b * rq_[poly_pos_].mob) * (transi * rq_[poly_pos_].dh);
                // Must recompute water flux since we have to use modified mobilities.
                rq_[ actph ].mflux = upwind.select(rq_[actph].b * rq_[actph].mob) * (transi * rq_[actph].dh);

                // applying the shear-thinning factors
                if (has_plyshlog_) {
                    V shear_mult_faces_v = Eigen::Map<V>(shear_mult_faces_.data(), shear_mult_faces_.size());
                    ADB shear_mult_faces_adb = ADB::constant(shear_mult_faces_v);
                    rq_[poly_pos_].mflux = rq_[poly_pos_].mflux / shear_mult_faces_adb;
                    rq_[actph].mflux = rq_[actph].mflux / shear_mult_faces_adb;
                }
            }
        }
    }





    template <class Grid>
    double
    BlackoilPolymerModel<Grid>::convergenceReduction(const Eigen::Array<double, Eigen::Dynamic, MaxNumPhases+1>& B,
                                                     const Eigen::Array<double, Eigen::Dynamic, MaxNumPhases+1>& tempV,
                                                     const Eigen::Array<double, Eigen::Dynamic, MaxNumPhases+1>& R,
                                                     std::array<double,MaxNumPhases+1>& R_sum,
                                                     std::array<double,MaxNumPhases+1>& maxCoeff,
                                                     std::array<double,MaxNumPhases+1>& B_avg,
                                                     std::vector<double>& maxNormWell,
                                                     int nc,
                                                     int nw) const
    {
        // Do the global reductions
#if HAVE_MPI
        if ( linsolver_.parallelInformation().type() == typeid(ParallelISTLInformation) )
        {
            const ParallelISTLInformation& info =
                boost::any_cast<const ParallelISTLInformation&>(linsolver_.parallelInformation());

            // Compute the global number of cells and porevolume
            std::vector<int> v(nc, 1);
            auto nc_and_pv = std::tuple<int, double>(0, 0.0);
            auto nc_and_pv_operators = std::make_tuple(Opm::Reduction::makeGlobalSumFunctor<int>(),
                                                        Opm::Reduction::makeGlobalSumFunctor<double>());
            auto nc_and_pv_containers  = std::make_tuple(v, geo_.poreVolume());
            info.computeReduction(nc_and_pv_containers, nc_and_pv_operators, nc_and_pv);

            for ( int idx=0; idx<MaxNumPhases+1; ++idx )
            {
                if ((idx == MaxNumPhases && has_polymer_) || active_[idx]) { // Dealing with polymer *or* an active phase.
                    auto values     = std::tuple<double,double,double>(0.0 ,0.0 ,0.0);
                    auto containers = std::make_tuple(B.col(idx),
                                                      tempV.col(idx),
                                                      R.col(idx));
                    auto operators  = std::make_tuple(Opm::Reduction::makeGlobalSumFunctor<double>(),
                                                      Opm::Reduction::makeGlobalMaxFunctor<double>(),
                                                      Opm::Reduction::makeGlobalSumFunctor<double>());
                    info.computeReduction(containers, operators, values);
                    B_avg[idx]       = std::get<0>(values)/std::get<0>(nc_and_pv);
                    maxCoeff[idx]    = std::get<1>(values);
                    R_sum[idx]       = std::get<2>(values);
                    if (idx != MaxNumPhases) { // We do not compute a well flux residual for polymer.
                        maxNormWell[idx] = 0.0;
                        for ( int w=0; w<nw; ++w ) {
                            maxNormWell[idx]  = std::max(maxNormWell[idx], std::abs(residual_.well_flux_eq.value()[nw*idx + w]));
                        }
                    }
                }
                else
                {
                    maxNormWell[idx] = R_sum[idx] = B_avg[idx] = maxCoeff[idx] = 0.0;
                }
            }
            info.communicator().max(&maxNormWell[0], MaxNumPhases+1);
            // Compute pore volume
            return std::get<1>(nc_and_pv);
        }
        else
#endif
        {
            for ( int idx=0; idx<MaxNumPhases+1; ++idx )
            {
                if ((idx == MaxNumPhases && has_polymer_) || active_[idx]) { // Dealing with polymer *or* an active phase.
                    B_avg[idx] = B.col(idx).sum()/nc;
                    maxCoeff[idx] = tempV.col(idx).maxCoeff();
                    R_sum[idx] = R.col(idx).sum();
                }
                else
                {
                    R_sum[idx] = B_avg[idx] = maxCoeff[idx] =0.0;
                }
                if (idx != MaxNumPhases) { // We do not compute a well flux residual for polymer.
                    maxNormWell[idx] = 0.0;
                    for ( int w=0; w<nw; ++w ) {
                        maxNormWell[idx]  = std::max(maxNormWell[idx], std::abs(residual_.well_flux_eq.value()[nw*idx + w]));
                    }
                }
            }
            // Compute total pore volume
            return geo_.poreVolume().sum();
        }
    }


    template <class Grid>
    void
    BlackoilPolymerModel<Grid>::assemble(const ReservoirState& reservoir_state,
                                         WellState& well_state,
                                         const bool initial_assembly)
    {
        using namespace Opm::AutoDiffGrid;

        // Possibly switch well controls and updating well state to
        // get reasonable initial conditions for the wells
        updateWellControls(well_state);

        // Create the primary variables.
        SolutionState state = variableState(reservoir_state, well_state);

        if (initial_assembly) {
            // Create the (constant, derivativeless) initial state.
            SolutionState state0 = state;
            makeConstantState(state0);
            // Compute initial accumulation contributions
            // and well connection pressures.
            computeAccum(state0, 0);
            computeWellConnectionPressures(state0, well_state);
        }

        // OPM_AD_DISKVAL(state.pressure);
        // OPM_AD_DISKVAL(state.saturation[0]);
        // OPM_AD_DISKVAL(state.saturation[1]);
        // OPM_AD_DISKVAL(state.saturation[2]);
        // OPM_AD_DISKVAL(state.rs);
        // OPM_AD_DISKVAL(state.rv);
        // OPM_AD_DISKVAL(state.qs);
        // OPM_AD_DISKVAL(state.bhp);

        // -------- Mass balance equations --------
        assembleMassBalanceEq(state);

        // -------- Well equations ----------
        V aliveWells;

        if (has_plyshlog_) {
            std::vector<double> water_vel_wells;
            std::vector<double> visc_mult_wells;

            computeWaterShearVelocityWells(state, well_state, aliveWells, water_vel_wells, visc_mult_wells);

            if (!computeShearMultLog(water_vel_wells, visc_mult_wells, shear_mult_wells_)) {
                // std::cout << " failed in calculating the shear factors for wells " << std::endl;
                OPM_THROW(std::runtime_error, " failed in calculating the shear factors for wells ");
            }

            /* const int nw = wells().number_of_wells;
            const int nperf = wells().well_connpos[nw];

            const std::vector<int> well_cells(wells().well_cells, wells().well_cells + nperf);

            // assuming the water phase is the first phase
            const int nw = wells().number_of_wells;
            mob_perfcells = subset(rq_[0].mob,well_cells); */
        }

        addWellEq(state, well_state, aliveWells);
        addWellControlEq(state, well_state, aliveWells);
    }




    template <class Grid>
    bool
    BlackoilPolymerModel<Grid>::getConvergence(const double dt, const int iteration)
    {
        const double tol_mb    = param_.tolerance_mb_;
        const double tol_cnv   = param_.tolerance_cnv_;
        const double tol_wells = param_.tolerance_wells_;

        const int nc = Opm::AutoDiffGrid::numCells(grid_);
        const int nw = wellsActive() ? wells().number_of_wells : 0;
        const Opm::PhaseUsage& pu = fluid_.phaseUsage();

        const V pv = geo_.poreVolume();

        const std::vector<PhasePresence> cond = phaseCondition();

        std::array<double,MaxNumPhases+1> CNV                   = {{0., 0., 0., 0.}};
        std::array<double,MaxNumPhases+1> R_sum                 = {{0., 0., 0., 0.}};
        std::array<double,MaxNumPhases+1> B_avg                 = {{0., 0., 0., 0.}};
        std::array<double,MaxNumPhases+1> maxCoeff              = {{0., 0., 0., 0.}};
        std::array<double,MaxNumPhases+1> mass_balance_residual = {{0., 0., 0., 0.}};
        std::array<double,MaxNumPhases> well_flux_residual    = {{0., 0., 0.}};
        std::size_t cols = MaxNumPhases+1; // needed to pass the correct type to Eigen
        Eigen::Array<V::Scalar, Eigen::Dynamic, MaxNumPhases+1> B(nc, cols);
        Eigen::Array<V::Scalar, Eigen::Dynamic, MaxNumPhases+1> R(nc, cols);
        Eigen::Array<V::Scalar, Eigen::Dynamic, MaxNumPhases+1> tempV(nc, cols);
        std::vector<double> maxNormWell(MaxNumPhases);

        for ( int idx=0; idx<MaxNumPhases; ++idx )
        {
            if (active_[idx]) {
                const int pos    = pu.phase_pos[idx];
                const ADB& tempB = rq_[pos].b;
                B.col(idx)       = 1./tempB.value();
                R.col(idx)       = residual_.material_balance_eq[idx].value();
                tempV.col(idx)   = R.col(idx).abs()/pv;
            }
        }
        if (has_polymer_) {
            const ADB& tempB = rq_[poly_pos_].b;
            B.col(MaxNumPhases) = 1. / tempB.value();
            R.col(MaxNumPhases) = residual_.material_balance_eq[poly_pos_].value();
            tempV.col(MaxNumPhases) = R.col(MaxNumPhases).abs()/pv;
        }

        const double pvSum = convergenceReduction(B, tempV, R, R_sum, maxCoeff, B_avg,
                                                  maxNormWell, nc, nw);

        bool converged_MB = true;
        bool converged_CNV = true;
        bool converged_Well = true;
        // Finish computation
        for ( int idx=0; idx<MaxNumPhases+1; ++idx )
        {
            CNV[idx]                   = B_avg[idx] * dt * maxCoeff[idx];
            mass_balance_residual[idx] = std::abs(B_avg[idx]*R_sum[idx]) * dt / pvSum;
            converged_MB               = converged_MB && (mass_balance_residual[idx] < tol_mb);
            converged_CNV              = converged_CNV && (CNV[idx] < tol_cnv);
            if (idx != MaxNumPhases) { // No well flux residual for polymer.
                well_flux_residual[idx]    = B_avg[idx] * dt * maxNormWell[idx];
                converged_Well = converged_Well && (well_flux_residual[idx] < tol_wells);
            }
        }

        const double residualWell     = detail::infinityNormWell(residual_.well_eq,
                                                                 linsolver_.parallelInformation());
        converged_Well   = converged_Well && (residualWell < Opm::unit::barsa);
        const bool   converged        = converged_MB && converged_CNV && converged_Well;

        // if one of the residuals is NaN, throw exception, so that the solver can be restarted
        if (std::isnan(mass_balance_residual[Water]) || mass_balance_residual[Water] > maxResidualAllowed() ||
            std::isnan(mass_balance_residual[Oil])   || mass_balance_residual[Oil]   > maxResidualAllowed() ||
            std::isnan(mass_balance_residual[Gas])   || mass_balance_residual[Gas]   > maxResidualAllowed() ||
            std::isnan(mass_balance_residual[MaxNumPhases])   || mass_balance_residual[MaxNumPhases]   > maxResidualAllowed() ||
            std::isnan(CNV[Water]) || CNV[Water] > maxResidualAllowed() ||
            std::isnan(CNV[Oil]) || CNV[Oil] > maxResidualAllowed() ||
            std::isnan(CNV[Gas]) || CNV[Gas] > maxResidualAllowed() ||
            std::isnan(CNV[MaxNumPhases]) || CNV[MaxNumPhases] > maxResidualAllowed() ||
            std::isnan(well_flux_residual[Water]) || well_flux_residual[Water] > maxResidualAllowed() ||
            std::isnan(well_flux_residual[Oil]) || well_flux_residual[Oil] > maxResidualAllowed() ||
            std::isnan(well_flux_residual[Gas]) || well_flux_residual[Gas] > maxResidualAllowed() ||
            std::isnan(residualWell)     || residualWell     > maxResidualAllowed() )
        {
            OPM_THROW(Opm::NumericalProblem,"One of the residuals is NaN or too large!");
        }

        if ( terminal_output_ )
        {
            // Only rank 0 does print to std::cout
            if (iteration == 0) {
                std::cout << "\nIter  MB(WATER)   MB(OIL)    MB(GAS)    MB(POLY)      CNVW       CNVO       CNVG       CNVP   W-FLUX(W)  W-FLUX(O)  W-FLUX(G)\n";
            }
            const std::streamsize oprec = std::cout.precision(3);
            const std::ios::fmtflags oflags = std::cout.setf(std::ios::scientific);
            std::cout << std::setw(4) << iteration
                      << std::setw(11) << mass_balance_residual[Water]
                      << std::setw(11) << mass_balance_residual[Oil]
                      << std::setw(11) << mass_balance_residual[Gas]
                      << std::setw(11) << mass_balance_residual[MaxNumPhases]
                      << std::setw(11) << CNV[Water]
                      << std::setw(11) << CNV[Oil]
                      << std::setw(11) << CNV[Gas]
                      << std::setw(11) << CNV[MaxNumPhases]
                      << std::setw(11) << well_flux_residual[Water]
                      << std::setw(11) << well_flux_residual[Oil]
                      << std::setw(11) << well_flux_residual[Gas]
                      << std::endl;
            std::cout.precision(oprec);
            std::cout.flags(oflags);
        }
        return converged;
    }

    template <class Grid>
    ADB
    BlackoilPolymerModel<Grid>::computeMc(const SolutionState& state) const
    {
        return polymer_props_ad_.polymerWaterVelocityRatio(state.concentration);
    }


    template <class Grid>
    void
    BlackoilPolymerModel<Grid>::addWellEq(const SolutionState& state,
                                                            WellState& xw,
                                                            V& aliveWells)
    {
        if( ! wellsActive() ) return ;

        const int nc = Opm::AutoDiffGrid::numCells(grid_);
        const int np = wells().number_of_phases;
        const int nw = wells().number_of_wells;
        const int nperf = wells().well_connpos[nw];
        const Opm::PhaseUsage& pu = fluid_.phaseUsage();
        V Tw = Eigen::Map<const V>(wells().WI, nperf);
        const std::vector<int> well_cells(wells().well_cells, wells().well_cells + nperf);

        // pressure diffs computed already (once per step, not changing per iteration)
        const V& cdp = well_perforation_pressure_diffs_;

        // Extract needed quantities for the perforation cells
        const ADB& p_perfcells = subset(state.pressure, well_cells);
        const ADB& rv_perfcells = subset(state.rv,well_cells);
        const ADB& rs_perfcells = subset(state.rs,well_cells);
        std::vector<ADB> mob_perfcells(np, ADB::null());
        std::vector<ADB> b_perfcells(np, ADB::null());
        for (int phase = 0; phase < np; ++phase) {
            mob_perfcells[phase] = subset(rq_[phase].mob,well_cells);
            b_perfcells[phase] = subset(rq_[phase].b,well_cells);
        }

        // applying the shear-thinning to the water face
        if (has_plyshlog_) {
            V shear_mult_wells_v = Eigen::Map<V>(shear_mult_wells_.data(), shear_mult_wells_.size());
            ADB shear_mult_wells_adb = ADB::constant(shear_mult_wells_v);
            mob_perfcells[0] = mob_perfcells[0] / shear_mult_wells_adb;
        }


        // Perforation pressure
        const ADB perfpressure = (wops_.w2p * state.bhp) + cdp;
        std::vector<double> perfpressure_d(perfpressure.value().data(), perfpressure.value().data() + nperf);
        xw.perfPress() = perfpressure_d;

        // Pressure drawdown (also used to determine direction of flow)
        const ADB drawdown =  p_perfcells - perfpressure;

        // Compute vectors with zero and ones that
        // selects the wanted quantities.

        // selects injection perforations
        V selectInjectingPerforations = V::Zero(nperf);
        // selects producing perforations
        V selectProducingPerforations = V::Zero(nperf);
        for (int c = 0; c < nperf; ++c){
            if (drawdown.value()[c] < 0)
                selectInjectingPerforations[c] = 1;
            else
                selectProducingPerforations[c] = 1;
        }   

        // HANDLE FLOW INTO WELLBORE

        // compute phase volumetric rates at standard conditions
        std::vector<ADB> cq_ps(np, ADB::null());
        for (int phase = 0; phase < np; ++phase) {
            const ADB cq_p = -(selectProducingPerforations * Tw) * (mob_perfcells[phase] * drawdown);
            cq_ps[phase] = b_perfcells[phase] * cq_p;
        }
        if (active_[Oil] && active_[Gas]) {
            const int oilpos = pu.phase_pos[Oil];
            const int gaspos = pu.phase_pos[Gas];
            const ADB cq_psOil = cq_ps[oilpos];
            const ADB cq_psGas = cq_ps[gaspos];
            cq_ps[gaspos] += rs_perfcells * cq_psOil;
            cq_ps[oilpos] += rv_perfcells * cq_psGas;
        }

        // HANDLE FLOW OUT FROM WELLBORE

        // Using total mobilities
        ADB total_mob = mob_perfcells[0];
        for (int phase = 1; phase < np; ++phase) {
            total_mob += mob_perfcells[phase];
        }
        // injection perforations total volume rates
        const ADB cqt_i = -(selectInjectingPerforations * Tw) * (total_mob * drawdown);

        // compute wellbore mixture for injecting perforations
        // The wellbore mixture depends on the inflow from the reservoar
        // and the well injection rates.

        // compute avg. and total wellbore phase volumetric rates at standard conds
        const DataBlock compi = Eigen::Map<const DataBlock>(wells().comp_frac, nw, np);
        std::vector<ADB> wbq(np, ADB::null());
        ADB wbqt = ADB::constant(V::Zero(nw));
        for (int phase = 0; phase < np; ++phase) {
            const ADB& q_ps = wops_.p2w * cq_ps[phase];
            const ADB& q_s = subset(state.qs, Span(nw, 1, phase*nw));
            Selector<double> injectingPhase_selector(q_s.value(), Selector<double>::GreaterZero);
            const int pos = pu.phase_pos[phase];
            wbq[phase] = (compi.col(pos) * injectingPhase_selector.select(q_s,ADB::constant(V::Zero(nw))))  - q_ps;
            wbqt += wbq[phase];
        }

        // compute wellbore mixture at standard conditions.
        Selector<double> notDeadWells_selector(wbqt.value(), Selector<double>::Zero);
        std::vector<ADB> cmix_s(np, ADB::null());
        for (int phase = 0; phase < np; ++phase) {
            const int pos = pu.phase_pos[phase];
            cmix_s[phase] = wops_.w2p * notDeadWells_selector.select(ADB::constant(compi.col(pos)), wbq[phase]/wbqt);
        }

        // compute volume ratio between connection at standard conditions
        ADB volumeRatio = ADB::constant(V::Zero(nperf));
        const ADB d = V::Constant(nperf,1.0) -  rv_perfcells * rs_perfcells;
        for (int phase = 0; phase < np; ++phase) {
            ADB tmp = cmix_s[phase];

            if (phase == Oil && active_[Gas]) {
                const int gaspos = pu.phase_pos[Gas];
                tmp = tmp - rv_perfcells * cmix_s[gaspos] / d;
            }
            if (phase == Gas && active_[Oil]) {
                const int oilpos = pu.phase_pos[Oil];
                tmp = tmp - rs_perfcells * cmix_s[oilpos] / d;
            }
            volumeRatio += tmp / b_perfcells[phase];
        }

        // injecting connections total volumerates at standard conditions
        ADB cqt_is = cqt_i/volumeRatio;

        // connection phase volumerates at standard conditions
        std::vector<ADB> cq_s(np, ADB::null());
        for (int phase = 0; phase < np; ++phase) {
            cq_s[phase] = cq_ps[phase] + cmix_s[phase]*cqt_is;
        }

        // Add well contributions to mass balance equations
        for (int phase = 0; phase < np; ++phase) {
            residual_.material_balance_eq[phase] -= superset(cq_s[phase],well_cells,nc);
        }

        // WELL EQUATIONS
        ADB qs = state.qs;
        for (int phase = 0; phase < np; ++phase) {
            qs -= superset(wops_.p2w * cq_s[phase], Span(nw, 1, phase*nw), nw*np);

        }

        // check for dead wells (used in the well controll equations)
        aliveWells = V::Constant(nw, 1.0);
        for (int w = 0; w < nw; ++w) {
            if (wbqt.value()[w] == 0) {
                aliveWells[w] = 0.0;
            }
        }

        // Update the perforation phase rates (used to calculate the pressure drop in the wellbore)
        V cq = superset(cq_s[0].value(), Span(nperf, np, 0), nperf*np);
        for (int phase = 1; phase < np; ++phase) {
            cq += superset(cq_s[phase].value(), Span(nperf, np, phase), nperf*np);
        }

        std::vector<double> cq_d(cq.data(), cq.data() + nperf*np);
        xw.perfPhaseRates() = cq_d;

        residual_.well_flux_eq = qs;

        extraAddWellEq(state, xw, cq_ps, cmix_s, cqt_is, well_cells);
    }


    template<class Grid>
    bool
    BlackoilPolymerModel<Grid>::findIntersection(Point2D line_segment1[2], Point2D line2[2], Point2D& intersection_point)
    {

        const double x1 = line_segment1[0].x;
        const double y1 = line_segment1[0].y;
        const double x2 = line_segment1[1].x;
        const double y2 = line_segment1[1].y;

        const double x3 = line2[0].x;
        const double y3 = line2[0].y;
        const double x4 = line2[1].x;
        const double y4 = line2[1].y;

        const double d = (x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4);

        if (d == 0.) {
            return false;
        }

        const double x = ((x3 - x4) * (x1 * y2 - y1 * x2) - (x1 - x2) * (x3 * y4 - y3 * x4)) / d;
        const double y = ((y3 - y4) * (x1 * y2 - y1 * x2) - (y1 - y2) * (x3 * y4 - y3 * x4)) / d;

        if (x >= std::min(x1,x2) && x <= std::max(x1,x2)) {
            intersection_point.x = x;
            intersection_point.y = y;
            return true;
        } else {
            return false;
        }
    }

    template<class Grid>
    bool
    BlackoilPolymerModel<Grid>::computeShearMultLog(std::vector<double>& water_vel, std::vector<double>& visc_mult, std::vector<double>& shear_mult)
    {

        double refConcentration = polymer_props_ad_.plyshlogRefConc();
        double refViscMult = polymer_props_ad_.viscMult(refConcentration);

        std::vector<double> shear_water_vel = polymer_props_ad_.shearWaterVelocity();
        std::vector<double> shear_vrf = polymer_props_ad_.shearViscosityReductionFactor();

        std::vector<double> logShearWaterVel;
        std::vector<double> logShearVRF;

        logShearWaterVel.resize(shear_water_vel.size());
        logShearVRF.resize(shear_water_vel.size());

        // converting the table using the reference condition
        for (int i = 0; i < shear_vrf.size(); ++i) {
            shear_vrf[i] = (refViscMult * shear_vrf[i] - 1.) / (refViscMult - 1);
            logShearWaterVel[i] = std::log(shear_water_vel[i]);
        }

        shear_mult.resize(water_vel.size());

        // the mimum velocity to apply the shear-thinning
        const double minShearVel = shear_water_vel[0];
        const double maxShearVel = shear_water_vel.back();
        const double epsilon = std::sqrt(std::numeric_limits<double>::epsilon());

        for (int i = 0; i < water_vel.size(); ++i) {

            if (visc_mult[i] - 1. < epsilon || std::abs(water_vel[i]) < minShearVel) {
                 shear_mult[i] = 1.0;
                 continue;
            }

            for (int j = 0; j < shear_vrf.size(); ++j) {
                logShearVRF[j] = (1 + (visc_mult[i] - 1.0) * shear_vrf[j]) / visc_mult[i];
                logShearVRF[j] = std::log(logShearVRF[j]);
            }

            // const double logWaterVelO = std::log(water_vel[i]);
            const double logWaterVelO = std::log(std::abs(water_vel[i]));

            int iIntersection; // finding the intersection on the iIntersectionth table segment
            bool foundSegment = false;

            for (iIntersection = 0; iIntersection < shear_vrf.size() - 1; ++iIntersection) {

                double temp1 = logShearVRF[iIntersection] + logShearWaterVel[iIntersection] - logWaterVelO;
                double temp2 = logShearVRF[iIntersection + 1] + logShearWaterVel[iIntersection + 1] - logWaterVelO;

                // ignore the cases the temp1 or temp2 is zero first for simplicity.
                // several more complicated cases remain to be implemented.
                if( temp1 * temp2 < 0.){
                    foundSegment = true;
                    break;
                }
            }

            if (foundSegment == true) {
                Point2D lineSegment[2];
                lineSegment[0] = Point2D{logShearWaterVel[iIntersection], logShearVRF[iIntersection]};
                lineSegment[1] = Point2D{logShearWaterVel[iIntersection + 1], logShearVRF[iIntersection + 1]};

                Point2D line[2];
                line[0] = Point2D{0, logWaterVelO};
                line[1] = Point2D{logWaterVelO, 0};

                Point2D intersectionPoint;

                bool foundIntersection = findIntersection(lineSegment, line, intersectionPoint);

                if (foundIntersection) {
                    shear_mult[i] = std::exp(intersectionPoint.y);
                } else {
                    std::cerr << " failed in finding the solution for shear-thinning multiplier " << std::endl;
                    return false; // failed in finding the solution.
                }
            } else {
                if (std::abs(water_vel[i]) < maxShearVel) {
                    std::cout << " the veclocity is " << water_vel[i] << std::endl;
                    std::cout << " max shear velocity is " << maxShearVel << std::endl;
                    std::cerr << " something wrong happend in finding segment" << std::endl;
                    return false;
                } else {
                    shear_mult[i] = std::exp(logShearVRF.back());
                }
            }

        }

        return true;
    }

    template<class Grid>
    void
    BlackoilPolymerModel<Grid>::computeWaterShearVelocityFaces(const V& transi, const std::vector<ADB>& kr,
                                                               const std::vector<ADB>& phasePressure, const SolutionState& state,
                                                               std::vector<double>& water_vel, std::vector<double>& visc_mult)
    {

        std::vector<double> b_faces;

        const int phase = fluid_.phaseUsage().phase_pos[BlackoilPhases::Aqua]; // water position

        const int canonicalPhaseIdx = canph_[phase];

        const std::vector<PhasePresence> cond = phaseCondition();

        const ADB tr_mult = transMult(state.pressure);
        const ADB mu    = fluidViscosity(canonicalPhaseIdx, phasePressure[canonicalPhaseIdx], state.temperature, state.rs, state.rv,cond, cells_);
        rq_[phase].mob = tr_mult * kr[canonicalPhaseIdx] / mu;

        // compute gravity potensial using the face average as in eclipse and MRST
        const ADB rho   = fluidDensity(canonicalPhaseIdx, phasePressure[canonicalPhaseIdx], state.temperature, state.rs, state.rv,cond, cells_);
        const ADB rhoavg = ops_.caver * rho;
        rq_[ phase ].dh = ops_.ngrad * phasePressure[ canonicalPhaseIdx ] - geo_.gravity()[2] * (rhoavg * (ops_.ngrad * geo_.z().matrix()));
        if (use_threshold_pressure_) {
            applyThresholdPressures(rq_[ phase ].dh);
        }

        const ADB& b   = rq_[ phase ].b;
        const ADB& mob = rq_[ phase ].mob;
        const ADB& dh  = rq_[ phase ].dh;
        UpwindSelector<double> upwind(grid_, ops_, dh.value());

        const ADB cmax = ADB::constant(cmax_, state.concentration.blockPattern());
        const ADB mc = computeMc(state);
        ADB krw_eff = polymer_props_ad_.effectiveRelPerm(state.concentration,
                                                         cmax,
                                                         kr[canonicalPhaseIdx]);
        ADB inv_wat_eff_visc = polymer_props_ad_.effectiveInvWaterVisc(state.concentration, mu.value().data());
        rq_[ phase ].mob = tr_mult * krw_eff * inv_wat_eff_visc;

        const V& polymer_conc = state.concentration.value();
        V visc_mult_cells = polymer_props_ad_.viscMult(polymer_conc);
        V visc_mult_faces = upwind.select(visc_mult_cells);

        size_t nface = visc_mult_faces.size();
        visc_mult.resize(nface);
        std::copy(&(visc_mult_faces[0]), &(visc_mult_faces[0]) + nface, visc_mult.begin());

        rq_[ phase ].mflux = upwind.select(b * mob) * (transi * dh);

        const auto& tempb_faces = upwind.select(b);
        b_faces.resize(tempb_faces.size());
        std::copy(&(tempb_faces.value()[0]), &(tempb_faces.value()[0]) + tempb_faces.size(), b_faces.begin());

        const auto& internal_faces = ops_.internal_faces;

        std::vector<double> internal_face_areas;
        internal_face_areas.resize(internal_faces.size());

        for (int i = 0; i < internal_faces.size(); ++i) {
            internal_face_areas[i] = grid_.face_areas[internal_faces[i]];
        }

        const ADB phi = Opm::AutoDiffBlock<double>::constant(Eigen::Map<const V>(& fluid_.porosity()[0], AutoDiffGrid::numCells(grid_), 1));
        const ADB temp_phiavg  = ops_.caver * phi;

        std::vector<double> phiavg;
        phiavg.resize(temp_phiavg.size());
        std::copy(&(temp_phiavg.value()[0]), &(temp_phiavg.value()[0]) + temp_phiavg.size(), phiavg.begin());

        water_vel.resize(nface);
        std::copy(&(rq_[0].mflux.value()[0]), &(rq_[0].mflux.value()[0]) + nface, water_vel.begin());

        for (int i = 0; i < nface; ++i) {
            water_vel[i] = water_vel[i] / (b_faces[i] * phiavg[i] * internal_face_areas[i]);
        }
    }

    template<class Grid>
    void
    BlackoilPolymerModel<Grid>::computeWaterShearVelocityWells(const SolutionState& state, WellState& xw,
                                                               V& aliveWells, std::vector<double>& water_vel_wells, std::vector<double>& visc_mult_wells)
    {
        if( ! wellsActive() ) return ;

        // const int nc = Opm::AutoDiffGrid::numCells(grid_);
        const int np = wells().number_of_phases;
        const int nw = wells().number_of_wells;
        const int nperf = wells().well_connpos[nw];
        const Opm::PhaseUsage& pu = fluid_.phaseUsage();
        V Tw = Eigen::Map<const V>(wells().WI, nperf);
        const std::vector<int> well_cells(wells().well_cells, wells().well_cells + nperf);

        // pressure diffs computed already (once per step, not changing per iteration)
        const V& cdp = well_perforation_pressure_diffs_;

        // Extract needed quantities for the perforation cells
        const ADB& p_perfcells = subset(state.pressure, well_cells);
        const ADB& rv_perfcells = subset(state.rv,well_cells);
        const ADB& rs_perfcells = subset(state.rs,well_cells);
        std::vector<ADB> mob_perfcells(np, ADB::null());
        std::vector<ADB> b_perfcells(np, ADB::null());
        for (int phase = 0; phase < np; ++phase) {
            mob_perfcells[phase] = subset(rq_[phase].mob,well_cells);
            b_perfcells[phase] = subset(rq_[phase].b,well_cells);
        }

        // Perforation pressure
        const ADB perfpressure = (wops_.w2p * state.bhp) + cdp;
        std::vector<double> perfpressure_d(perfpressure.value().data(), perfpressure.value().data() + nperf);
        xw.perfPress() = perfpressure_d;

        // Pressure drawdown (also used to determine direction of flow)
        const ADB drawdown =  p_perfcells - perfpressure;

        // Compute vectors with zero and ones that
        // selects the wanted quantities.

        // selects injection perforations
        V selectInjectingPerforations = V::Zero(nperf);
        // selects producing perforations
        V selectProducingPerforations = V::Zero(nperf);
        for (int c = 0; c < nperf; ++c) {
            if (drawdown.value()[c] < 0)
                selectInjectingPerforations[c] = 1;
            else
                selectProducingPerforations[c] = 1;
        }

        // HANDLE FLOW INTO WELLBORE

        // compute phase volumetric rates at standard conditions
        std::vector<ADB> cq_ps(np, ADB::null());
        for (int phase = 0; phase < np; ++phase) {
            const ADB cq_p = -(selectProducingPerforations * Tw) * (mob_perfcells[phase] * drawdown);
            cq_ps[phase] = b_perfcells[phase] * cq_p;
        }
        if (active_[Oil] && active_[Gas]) {
            const int oilpos = pu.phase_pos[Oil];
            const int gaspos = pu.phase_pos[Gas];
            const ADB cq_psOil = cq_ps[oilpos];
            const ADB cq_psGas = cq_ps[gaspos];
            cq_ps[gaspos] += rs_perfcells * cq_psOil;
            cq_ps[oilpos] += rv_perfcells * cq_psGas;
        }

        // HANDLE FLOW OUT FROM WELLBORE

        // Using total mobilities
        ADB total_mob = mob_perfcells[0];
        for (int phase = 1; phase < np; ++phase) {
            total_mob += mob_perfcells[phase];
        }
        // injection perforations total volume rates
        const ADB cqt_i = -(selectInjectingPerforations * Tw) * (total_mob * drawdown);

        // compute wellbore mixture for injecting perforations
        // The wellbore mixture depends on the inflow from the reservoar
        // and the well injection rates.

        // compute avg. and total wellbore phase volumetric rates at standard conds
        const DataBlock compi = Eigen::Map<const DataBlock>(wells().comp_frac, nw, np);
        std::vector<ADB> wbq(np, ADB::null());
        ADB wbqt = ADB::constant(V::Zero(nw));
        for (int phase = 0; phase < np; ++phase) {
            const ADB& q_ps = wops_.p2w * cq_ps[phase];
            const ADB& q_s = subset(state.qs, Span(nw, 1, phase*nw));
            Selector<double> injectingPhase_selector(q_s.value(), Selector<double>::GreaterZero);
            const int pos = pu.phase_pos[phase];
            wbq[phase] = (compi.col(pos) * injectingPhase_selector.select(q_s,ADB::constant(V::Zero(nw))))  - q_ps;
            wbqt += wbq[phase];
        }

        // compute wellbore mixture at standard conditions.
        Selector<double> notDeadWells_selector(wbqt.value(), Selector<double>::Zero);
        std::vector<ADB> cmix_s(np, ADB::null());
        for (int phase = 0; phase < np; ++phase) {
            const int pos = pu.phase_pos[phase];
            cmix_s[phase] = wops_.w2p * notDeadWells_selector.select(ADB::constant(compi.col(pos)), wbq[phase]/wbqt);
        }

        // compute volume ratio between connection at standard conditions
        ADB volumeRatio = ADB::constant(V::Zero(nperf));
        const ADB d = V::Constant(nperf,1.0) -  rv_perfcells * rs_perfcells;
        for (int phase = 0; phase < np; ++phase) {
            ADB tmp = cmix_s[phase];

            if (phase == Oil && active_[Gas]) {
                const int gaspos = pu.phase_pos[Gas];
                tmp = tmp - rv_perfcells * cmix_s[gaspos] / d;
            }
            if (phase == Gas && active_[Oil]) {
                const int oilpos = pu.phase_pos[Oil];
                tmp = tmp - rs_perfcells * cmix_s[oilpos] / d;
            }
            volumeRatio += tmp / b_perfcells[phase];
        }

        // injecting connections total volumerates at standard conditions
        ADB cqt_is = cqt_i/volumeRatio;

        // connection phase volumerates at standard conditions
        std::vector<ADB> cq_s(np, ADB::null());
        for (int phase = 0; phase < np; ++phase) {
            cq_s[phase] = cq_ps[phase] + cmix_s[phase]*cqt_is;
        }

        water_vel_wells.resize(cq_s[0].size());
        std::copy(&(cq_s[0].value()[0]), &(cq_s[0].value()[0]) + cq_s[0].size(), water_vel_wells.begin());


        const V& polymer_conc = state.concentration.value();

        V visc_mult_cells = polymer_props_ad_.viscMult(polymer_conc);

        V temp_visc_mult_wells = subset(visc_mult_cells, well_cells);

        visc_mult_wells.resize(temp_visc_mult_wells.size());
        std::copy(&(temp_visc_mult_wells[0]), &(temp_visc_mult_wells[0]) + temp_visc_mult_wells.size(), visc_mult_wells.begin());

        // for the injection wells
        for (int i = 0; i < well_cells.size(); ++i) {
            if (xw.polymerInflow()[well_cells[i]] == 0. && selectInjectingPerforations[i] == 1) { // maybe comparison with epsilon threshold
                visc_mult_wells[i] = 1.;
            }
        }

        const ADB phi = Opm::AutoDiffBlock<double>::constant(Eigen::Map<const V>(& fluid_.porosity()[0], AutoDiffGrid::numCells(grid_), 1));

        const ADB temp_phi_wells = subset(phi, well_cells);

        std::vector<double> phi_wells;
        phi_wells.resize(temp_phi_wells.size());

        std::copy(&(temp_phi_wells.value()[0]), &(temp_phi_wells.value()[0]) + temp_phi_wells.size(), phi_wells.begin());

        std::vector<double> b_wells;

        b_wells.resize(b_perfcells[0].size());
        std::copy(&(b_perfcells[0].value()[0]), &(b_perfcells[0].value()[0]) + b_perfcells[0].size(), b_wells.begin());

        for (int i = 0; i < water_vel_wells.size(); ++i) {
            water_vel_wells[i] = b_wells[i] * water_vel_wells[i] / (phi_wells[i] * 2. * M_PI * wells_rep_radius_[i] * wells_perf_length_[i]);
            // TODO: CHECK to make sure this formulation is corectly used. Why muliplied by bW.
            // Although this formulation works perfectly with the tests compared with other formulations
        }

        return;
    }

} // namespace Opm

#endif // OPM_BLACKOILPOLYMERMODEL_IMPL_HEADER_INCLUDED