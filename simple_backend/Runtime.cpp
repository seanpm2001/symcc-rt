#include <Runtime.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

#include "Config.h"
#include "LibcWrappers.h"
#include "Shadow.h"

#ifdef DEBUG_RUNTIME
// Helper to print pointers properly.
#define P(ptr) reinterpret_cast<void *>(ptr)
#endif

#define FSORT(is_double)                                                       \
  (is_double ? Z3_mk_fpa_sort_double(g_context)                                \
             : Z3_mk_fpa_sort_single(g_context))

/* TODO Eventually we'll want to inline as much of this as possible. I'm keeping
   it in C for now because that makes it easier to experiment with new features,
   but I expect that a lot of the functions will stay so simple that we can
   generate the corresponding bitcode directly in the compiler pass. */

namespace {

/// Indicate whether the runtime has been initialized.
std::atomic_flag g_initialized = ATOMIC_FLAG_INIT;

/// The global Z3 context.
Z3_context g_context;

/// The global floating-point rounding mode.
Z3_ast g_rounding_mode;

/// The global Z3 solver.
Z3_solver g_solver; // TODO make thread-local

// Some global constants for efficiency.
Z3_ast g_null_pointer, g_true, g_false;

#ifdef DEBUG_RUNTIME
[[maybe_unused]] void dump_known_regions() {
  std::cout << "Known regions:" << std::endl;
  for (const auto &[page, shadow] : g_shadow_pages) {
    std::cout << "  " << P(page) << " shadowed by " << P(shadow) << std::endl;
  }
}

void handle_z3_error(Z3_context c [[maybe_unused]], Z3_error_code e) {
  assert(c == g_context && "Z3 error in unknown context");
  std::cerr << Z3_get_error_msg(g_context, e) << std::endl;
  assert(!"Z3 error");
}
#endif

Z3_ast build_variable(const char *name, uint8_t bits) {
  Z3_symbol sym = Z3_mk_string_symbol(g_context, name);
  return Z3_mk_const(g_context, sym, Z3_mk_bv_sort(g_context, bits));
}

} // namespace

void _sym_initialize(void) {
  if (g_initialized.test_and_set())
    return;

#ifdef DEBUG_RUNTIME
  std::cout << "Initializing symbolic runtime" << std::endl;
#endif

  loadConfig();
  initLibcWrappers();

  Z3_config cfg;

  cfg = Z3_mk_config();
  Z3_set_param_value(cfg, "model", "true");
  Z3_set_param_value(cfg, "timeout", "10000"); // milliseconds
  g_context = Z3_mk_context(cfg);
  Z3_del_config(cfg);

#ifdef DEBUG_RUNTIME
  Z3_set_error_handler(g_context, handle_z3_error);
#endif

  g_rounding_mode = Z3_mk_fpa_round_nearest_ties_to_even(g_context);

  g_solver = Z3_mk_solver(g_context);
  Z3_solver_inc_ref(g_context, g_solver);

  g_null_pointer =
      Z3_mk_int(g_context, 0, Z3_mk_bv_sort(g_context, 8 * sizeof(void *)));
  g_true = Z3_mk_true(g_context);
  g_false = Z3_mk_false(g_context);
}

Z3_ast _sym_build_integer(uint64_t value, uint8_t bits) {
  return Z3_mk_unsigned_int64(g_context, value, Z3_mk_bv_sort(g_context, bits));
}

Z3_ast _sym_build_integer128(unsigned __int128 value) {
  return Z3_mk_concat(g_context, _sym_build_integer((value >> 64), 64),
                      _sym_build_integer(value, 64));
}

Z3_ast _sym_build_float(double value, int is_double) {
  return Z3_mk_fpa_numeral_double(g_context, value,
                                  is_double ? Z3_mk_fpa_sort_double(g_context)
                                            : Z3_mk_fpa_sort_single(g_context));
}

Z3_ast _sym_get_input_byte(size_t offset) {
  static std::vector<SymExpr> stdinBytes;

  if (offset < stdinBytes.size())
    return stdinBytes[offset];

  auto varName = "stdin" + std::to_string(stdinBytes.size());
  auto var = build_variable(varName.c_str(), 8);

  stdinBytes.resize(offset);
  stdinBytes.push_back(var);

  return var;
}

Z3_ast _sym_build_null_pointer(void) { return g_null_pointer; }
Z3_ast _sym_build_true(void) { return g_true; }
Z3_ast _sym_build_false(void) { return g_false; }
Z3_ast _sym_build_bool(bool value) { return value ? g_true : g_false; }

Z3_ast _sym_build_add(Z3_ast a, Z3_ast b) {
  return Z3_mk_bvadd(g_context, a, b);
}

Z3_ast _sym_build_sub(Z3_ast a, Z3_ast b) {
  return Z3_mk_bvsub(g_context, a, b);
}

Z3_ast _sym_build_mul(Z3_ast a, Z3_ast b) {
  return Z3_mk_bvmul(g_context, a, b);
}

Z3_ast _sym_build_unsigned_div(Z3_ast a, Z3_ast b) {
  return Z3_mk_bvudiv(g_context, a, b);
}

Z3_ast _sym_build_signed_div(Z3_ast a, Z3_ast b) {
  return Z3_mk_bvsdiv(g_context, a, b);
}

Z3_ast _sym_build_unsigned_rem(Z3_ast a, Z3_ast b) {
  return Z3_mk_bvurem(g_context, a, b);
}

Z3_ast _sym_build_signed_rem(Z3_ast a, Z3_ast b) {
  return Z3_mk_bvsrem(g_context, a, b);
}

Z3_ast _sym_build_shift_left(Z3_ast a, Z3_ast b) {
  return Z3_mk_bvshl(g_context, a, b);
}

Z3_ast _sym_build_logical_shift_right(Z3_ast a, Z3_ast b) {
  return Z3_mk_bvlshr(g_context, a, b);
}

Z3_ast _sym_build_arithmetic_shift_right(Z3_ast a, Z3_ast b) {
  return Z3_mk_bvashr(g_context, a, b);
}

Z3_ast _sym_build_fp_add(Z3_ast a, Z3_ast b) {
  return Z3_mk_fpa_add(g_context, g_rounding_mode, a, b);
}

Z3_ast _sym_build_fp_sub(Z3_ast a, Z3_ast b) {
  return Z3_mk_fpa_sub(g_context, g_rounding_mode, a, b);
}

Z3_ast _sym_build_fp_mul(Z3_ast a, Z3_ast b) {
  return Z3_mk_fpa_mul(g_context, g_rounding_mode, a, b);
}

Z3_ast _sym_build_fp_div(Z3_ast a, Z3_ast b) {
  return Z3_mk_fpa_div(g_context, g_rounding_mode, a, b);
}

Z3_ast _sym_build_fp_rem(Z3_ast a, Z3_ast b) {
  return Z3_mk_fpa_rem(g_context, a, b);
}

Z3_ast _sym_build_fp_abs(Z3_ast a) { return Z3_mk_fpa_abs(g_context, a); }

Z3_ast _sym_build_neg(Z3_ast expr) { return Z3_mk_not(g_context, expr); }

Z3_ast _sym_build_signed_less_than(Z3_ast a, Z3_ast b) {
  return Z3_mk_bvslt(g_context, a, b);
}

Z3_ast _sym_build_signed_less_equal(Z3_ast a, Z3_ast b) {
  return Z3_mk_bvsle(g_context, a, b);
}

Z3_ast _sym_build_signed_greater_than(Z3_ast a, Z3_ast b) {
  return Z3_mk_bvsgt(g_context, a, b);
}

Z3_ast _sym_build_signed_greater_equal(Z3_ast a, Z3_ast b) {
  return Z3_mk_bvsge(g_context, a, b);
}

Z3_ast _sym_build_unsigned_less_than(Z3_ast a, Z3_ast b) {
  return Z3_mk_bvult(g_context, a, b);
}

Z3_ast _sym_build_unsigned_less_equal(Z3_ast a, Z3_ast b) {
  return Z3_mk_bvule(g_context, a, b);
}

Z3_ast _sym_build_unsigned_greater_than(Z3_ast a, Z3_ast b) {
  return Z3_mk_bvugt(g_context, a, b);
}

Z3_ast _sym_build_unsigned_greater_equal(Z3_ast a, Z3_ast b) {
  return Z3_mk_bvuge(g_context, a, b);
}

Z3_ast _sym_build_equal(Z3_ast a, Z3_ast b) {
  return Z3_mk_eq(g_context, a, b);
}

Z3_ast _sym_build_not_equal(Z3_ast a, Z3_ast b) {
  return Z3_mk_not(g_context, _sym_build_equal(a, b));
}

Z3_ast _sym_build_bool_and(Z3_ast a, Z3_ast b) {
  Z3_ast operands[] = {a, b};
  return Z3_mk_and(g_context, 2, operands);
}

Z3_ast _sym_build_and(Z3_ast a, Z3_ast b) {
  return Z3_mk_bvand(g_context, a, b);
}

Z3_ast _sym_build_bool_or(Z3_ast a, Z3_ast b) {
  Z3_ast operands[] = {a, b};
  return Z3_mk_or(g_context, 2, operands);
}

Z3_ast _sym_build_or(Z3_ast a, Z3_ast b) { return Z3_mk_bvor(g_context, a, b); }

Z3_ast _sym_build_bool_xor(Z3_ast a, Z3_ast b) {
  return Z3_mk_xor(g_context, a, b);
}

Z3_ast _sym_build_xor(Z3_ast a, Z3_ast b) {
  return Z3_mk_bvxor(g_context, a, b);
}

Z3_ast _sym_build_float_ordered_greater_than(Z3_ast a, Z3_ast b) {
  return Z3_mk_fpa_gt(g_context, a, b);
}

Z3_ast _sym_build_float_ordered_greater_equal(Z3_ast a, Z3_ast b) {
  return Z3_mk_fpa_geq(g_context, a, b);
}

Z3_ast _sym_build_float_ordered_less_than(Z3_ast a, Z3_ast b) {
  return Z3_mk_fpa_lt(g_context, a, b);
}

Z3_ast _sym_build_float_ordered_less_equal(Z3_ast a, Z3_ast b) {
  return Z3_mk_fpa_leq(g_context, a, b);
}

Z3_ast _sym_build_float_ordered_equal(Z3_ast a, Z3_ast b) {
  return Z3_mk_fpa_eq(g_context, a, b);
}

Z3_ast _sym_build_float_ordered_not_equal(Z3_ast a, Z3_ast b) {
  return Z3_mk_not(g_context, _sym_build_float_ordered_equal(a, b));
}

Z3_ast _sym_build_float_ordered(Z3_ast a, Z3_ast b) {
  return Z3_mk_not(g_context, _sym_build_float_unordered(a, b));
}

Z3_ast _sym_build_float_unordered(Z3_ast a, Z3_ast b) {
  Z3_ast checks[2];
  checks[0] = Z3_mk_fpa_is_nan(g_context, a);
  checks[1] = Z3_mk_fpa_is_nan(g_context, b);
  return Z3_mk_or(g_context, 2, checks);
}

Z3_ast _sym_build_float_unordered_greater_than(Z3_ast a, Z3_ast b) {
  Z3_ast checks[3];
  checks[0] = Z3_mk_fpa_is_nan(g_context, a);
  checks[1] = Z3_mk_fpa_is_nan(g_context, b);
  checks[2] = _sym_build_float_ordered_greater_than(a, b);
  return Z3_mk_or(g_context, 2, checks);
}

Z3_ast _sym_build_float_unordered_greater_equal(Z3_ast a, Z3_ast b) {
  Z3_ast checks[3];
  checks[0] = Z3_mk_fpa_is_nan(g_context, a);
  checks[1] = Z3_mk_fpa_is_nan(g_context, b);
  checks[2] = _sym_build_float_ordered_greater_equal(a, b);
  return Z3_mk_or(g_context, 2, checks);
}

Z3_ast _sym_build_float_unordered_less_than(Z3_ast a, Z3_ast b) {
  Z3_ast checks[3];
  checks[0] = Z3_mk_fpa_is_nan(g_context, a);
  checks[1] = Z3_mk_fpa_is_nan(g_context, b);
  checks[2] = _sym_build_float_ordered_less_than(a, b);
  return Z3_mk_or(g_context, 2, checks);
}

Z3_ast _sym_build_float_unordered_less_equal(Z3_ast a, Z3_ast b) {
  Z3_ast checks[3];
  checks[0] = Z3_mk_fpa_is_nan(g_context, a);
  checks[1] = Z3_mk_fpa_is_nan(g_context, b);
  checks[2] = _sym_build_float_ordered_less_equal(a, b);
  return Z3_mk_or(g_context, 2, checks);
}

Z3_ast _sym_build_float_unordered_equal(Z3_ast a, Z3_ast b) {
  Z3_ast checks[3];
  checks[0] = Z3_mk_fpa_is_nan(g_context, a);
  checks[1] = Z3_mk_fpa_is_nan(g_context, b);
  checks[2] = _sym_build_float_ordered_equal(a, b);
  return Z3_mk_or(g_context, 2, checks);
}

Z3_ast _sym_build_float_unordered_not_equal(Z3_ast a, Z3_ast b) {
  Z3_ast checks[3];
  checks[0] = Z3_mk_fpa_is_nan(g_context, a);
  checks[1] = Z3_mk_fpa_is_nan(g_context, b);
  checks[2] = _sym_build_float_ordered_not_equal(a, b);
  return Z3_mk_or(g_context, 2, checks);
}

Z3_ast _sym_build_sext(Z3_ast expr, uint8_t bits) {
  return Z3_mk_sign_ext(g_context, bits, expr);
}

Z3_ast _sym_build_zext(Z3_ast expr, uint8_t bits) {
  return Z3_mk_zero_ext(g_context, bits, expr);
}

Z3_ast _sym_build_trunc(Z3_ast expr, uint8_t bits) {
  return Z3_mk_extract(g_context, bits - 1, 0, expr);
}

Z3_ast _sym_build_int_to_float(Z3_ast value, int is_double, int is_signed) {
  return is_signed ? Z3_mk_fpa_to_fp_signed(g_context, g_rounding_mode, value,
                                            FSORT(is_double))
                   : Z3_mk_fpa_to_fp_unsigned(g_context, g_rounding_mode, value,
                                              FSORT(is_double));
}

Z3_ast _sym_build_float_to_float(Z3_ast expr, int to_double) {
  return Z3_mk_fpa_to_fp_float(g_context, g_rounding_mode, expr,
                               FSORT(to_double));
}

Z3_ast _sym_build_bits_to_float(Z3_ast expr, int to_double) {
  if (expr == nullptr)
    return nullptr;
  return Z3_mk_fpa_to_fp_bv(g_context, expr, FSORT(to_double));
}

Z3_ast _sym_build_float_to_bits(Z3_ast expr) {
  if (expr == nullptr)
    return nullptr;
  return Z3_mk_fpa_to_ieee_bv(g_context, expr);
}

Z3_ast _sym_build_float_to_signed_integer(Z3_ast expr, uint8_t bits) {
  return Z3_mk_fpa_to_sbv(g_context, Z3_mk_fpa_round_toward_zero(g_context),
                          expr, bits);
}

Z3_ast _sym_build_float_to_unsigned_integer(Z3_ast expr, uint8_t bits) {
  return Z3_mk_fpa_to_ubv(g_context, Z3_mk_fpa_round_toward_zero(g_context),
                          expr, bits);
}

Z3_ast _sym_build_bool_to_bits(Z3_ast expr, uint8_t bits) {
  return Z3_mk_ite(g_context, expr, _sym_build_integer(1, bits),
                   _sym_build_integer(0, bits));
}

void _sym_push_path_constraint(Z3_ast constraint, int taken,
                               uintptr_t site_id [[maybe_unused]]) {
  if (!constraint)
    return;

  constraint = Z3_simplify(g_context, constraint);

  /* Check the easy cases first: if simplification reduced the constraint to
     "true" or "false", there is no point in trying to solve the negation or *
     pushing the constraint to the solver... */

  if (Z3_is_eq_ast(g_context, constraint, Z3_mk_true(g_context))) {
    assert(taken && "We have taken an impossible branch");
    return;
  }

  if (Z3_is_eq_ast(g_context, constraint, Z3_mk_false(g_context))) {
    assert(!taken && "We have taken an impossible branch");
    return;
  }

  /* Generate a solution for the alternative */
  Z3_ast not_constraint =
      Z3_simplify(g_context, Z3_mk_not(g_context, constraint));

  Z3_solver_push(g_context, g_solver);
  Z3_solver_assert(g_context, g_solver, taken ? not_constraint : constraint);
  printf("Trying to solve:\n%s\n", Z3_solver_to_string(g_context, g_solver));

  Z3_lbool feasible = Z3_solver_check(g_context, g_solver);
  if (feasible == Z3_L_TRUE) {
    Z3_model model = Z3_solver_get_model(g_context, g_solver);
    Z3_model_inc_ref(g_context, model);
    printf("Found diverging input:\n%s\n",
           Z3_model_to_string(g_context, model));
    Z3_model_dec_ref(g_context, model);
  } else {
    printf("Can't find a diverging input at this point\n");
  }

  Z3_solver_pop(g_context, g_solver, 1);

  /* Assert the actual path constraint */
  Z3_ast newConstraint = (taken ? constraint : not_constraint);
  Z3_solver_assert(g_context, g_solver, newConstraint);
  assert((Z3_solver_check(g_context, g_solver) == Z3_L_TRUE) &&
         "Asserting infeasible path constraint");
}

SymExpr _sym_concat_helper(SymExpr a, SymExpr b) {
  return Z3_mk_concat(g_context, a, b);
}

SymExpr _sym_extract_helper(SymExpr expr, size_t first_bit, size_t last_bit) {
  return Z3_mk_extract(g_context, first_bit, last_bit, expr);
}

size_t _sym_bits_helper(SymExpr expr) {
  return Z3_get_bv_sort_size(g_context, Z3_get_sort(g_context, expr));
}

/* No call-stack tracing */
void _sym_notify_call(uintptr_t) {}
void _sym_notify_ret(uintptr_t) {}
void _sym_notify_basic_block(uintptr_t) {}

/* Debugging */
void _sym_print(SymExpr expr) {
  puts(Z3_ast_to_string(g_context, expr));
}
