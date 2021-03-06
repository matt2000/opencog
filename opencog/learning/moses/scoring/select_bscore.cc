/*
 * opencog/learning/moses/moses/select_bscore.cc
 *
 * Copyright (C) 2002-2008 Novamente LLC
 * Copyright (C) 2012,2013 Poulin Holdings LLC
 * Copyright (C) 2014 Aidyia Limited
 * All Rights Reserved
 *
 * Written by Moshe Looks, Nil Geisweiller, Linas Vepstas
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License v3 as
 * published by the Free Software Foundation and including the exceptions
 * at http://opencog.org/wiki/Licenses
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program; if not, write to:
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "select_bscore.h"

namespace opencog { namespace moses {

using namespace combo;

/**
 * Return the highest-weighted value (and its weight) in a given CTable row.
 *
 * CTable rows may have multiple, weighted output values. Several of
 * these output values may have the same weight, and only one of these
 * will be the largest. Find and return that.
 */
std::pair<double, double> 
select_bscore::get_weightiest(const Counter<vertex, count_t>& orow) const
{
    score_t weightiest_val = very_worst_score;
    score_t weightiest = 0.0;
    for (const CTable::counter_t::value_type& tcv : orow) {
        score_t val = get_contin(tcv.first);
        if (not _positive) val = -val;
        // The weight for a given value is in tcv.second.
        // We look for weightiest, or, if tied, then for the
        // largest value. This gives a unique sort order, even
        // for randomly re-ordereed tables.
        if (weightiest < tcv.second or 
            (weightiest == tcv.second and weightiest_val < val))
        {
            weightiest = tcv.second;
            weightiest_val = val;
        }
    }
    return std::pair<double, double>(weightiest_val, weightiest);
}

select_bscore::select_bscore(const CTable& ctable,
                             double lower_percentile,
                             double upper_percentile,
                             double hardness,
                             bool positive) :
    bscore_ctable_base(ctable), _positive(positive)
{
    OC_ASSERT(id::contin_type == _wrk_ctable.get_output_type(),
        "The selection scorer can only be used with contin-valued tables!");

    // Verify that the bounds are sane
    OC_ASSERT((0.0 <= lower_percentile) and
              (lower_percentile < 1.0) and
              (0.0 < upper_percentile) and
              (upper_percentile <= 1.0) and
              (0.0 <= hardness) and
              (hardness <= 1.0),
        "Selection scorer, invalid bounds.");

    // Maps are implicitly ordered, so the below has the effect of
    // putting the rows into sorted order, by output score.
    std::map<score_t, const CTable::value_type&> ranked_out;

    score_t total_weight = 0.0;
    for (const CTable::value_type& io_row : ctable) {
        // io_row.first = input vector
        // io_row.second = counter of outputs
        auto w = get_weightiest(io_row.second);
        score_t weightiest_val = w.first;
        score_t weightiest = w.second;

        ranked_out.insert(std::pair<score_t, const CTable::value_type&>(weightiest_val, io_row));
        total_weight += weightiest;
    }

    logger().info() << "select_bscore: lower_percentile = " << lower_percentile
                    << " upper percentile = " << upper_percentile
                    << " total wieght = " << total_weight;

    // Now, carve out the selected range.
    upper_percentile *= total_weight;
    lower_percentile *= total_weight;
    score_t running_weight = 0.0;
    bool found_lower = false;
    bool found_upper = false;
    for (auto score_row : ranked_out) {
        score_t weightiest_val = score_row.first;

        auto w = get_weightiest(score_row.second.second);
        score_t weightiest = w.second;
        running_weight += weightiest;

        if (not found_lower and lower_percentile < running_weight) {
            _lower_bound = weightiest_val;
            found_lower = true;
        }
        if (not found_upper and upper_percentile < running_weight) {
            _upper_bound = weightiest_val;
            found_upper = true;
        }
    }

    logger().info() << "select_bscore: lower_bound = " << _lower_bound
                    << " upper bound = " << _upper_bound;
}


behavioral_score select_bscore::operator()(const combo_tree& tr) const
{
    behavioral_score bs;

    interpreter_visitor iv(tr);
    auto interpret_tr = boost::apply_visitor(iv);

    for (const CTable::value_type& io_row : _wrk_ctable) {
        // io_row.first = input vector
        // io_row.second = counter of outputs
        auto w = get_weightiest(io_row.second);
        score_t weightiest_val = w.first;
        score_t weightiest = w.second;

        if (interpret_tr(io_row.first.get_variant()) == id::logical_true) {
            if (_lower_bound <= weightiest_val and weightiest_val <= _upper_bound)
                bs.push_back(weightiest);
            else
                bs.push_back(-weightiest);
        } else {
            if (_lower_bound <= weightiest_val and weightiest_val <= _upper_bound)
                bs.push_back(-weightiest);
            else
                bs.push_back(weightiest);
        }
    }

    return bs;
}

behavioral_score select_bscore::operator()(const scored_combo_tree_set& ensemble) const
{
    // Step 1: accumulate the weighted prediction of each tree in
    // the ensemble.
    behavioral_score hypoth (_size);
    for (const scored_combo_tree& sct: ensemble) {
        const combo_tree& tr = sct.get_tree();
        score_t trweight = sct.get_weight();

        interpreter_visitor iv(tr);
        auto interpret_tr = boost::apply_visitor(iv);

        size_t i=0;
        for (const CTable::value_type& io_row : _wrk_ctable) {
            // io_row.first = input vector
            bool select = (interpret_tr(io_row.first.get_variant()) == id::logical_true);
            hypoth[i] += trweight * (2.0 * ((score_t) select) - 1.0);
            i++;
        }
    }

    // Step 2: compare the prediction of the ensemble to the desired
    // result. The array "hypoth" is positive to predict true, and
    // negative to predict false.  The resulting score is 0 if corrrect,
    // and -1 if incorrect.
    behavioral_score bs;
    size_t i = 0;
    for (const CTable::value_type& io_row : _wrk_ctable) {
        // io_row.first = input vector
        // io_row.second = counter of outputs
        auto w = get_weightiest(io_row.second);
        score_t weightiest_val = w.first;
        score_t weightiest = w.second;

        if (0 < hypoth[i]) {
            if (_lower_bound <= weightiest_val and weightiest_val <= _upper_bound)
                bs.push_back(weightiest);
            else
                bs.push_back(-weightiest);
        } else {
            if (_lower_bound <= weightiest_val and weightiest_val <= _upper_bound)
                bs.push_back(-weightiest);
            else
                bs.push_back(weightiest);
        }
        i++;
    }

    return bs;
}


behavioral_score select_bscore::best_possible_bscore() const
{
    behavioral_score bs;

    for (const CTable::value_type& io_row : _wrk_ctable) {
        // io_row.first = input vector
        // io_row.second = counter of outputs
        auto w = get_weightiest(io_row.second);
        score_t weightiest = w.second;

        bs.push_back(weightiest);
    }
    return bs;
}


score_t select_bscore::min_improv() const
{
    return 1.0 / _ctable_usize;
}

} // ~namespace moses
} // ~namespace opencog
