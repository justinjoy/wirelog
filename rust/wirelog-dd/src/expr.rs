/*
 * expr.rs - RPN expression deserializer and stack-based evaluator
 *
 * The wirelog C side serializes IR expression trees into flat RPN (Reverse Polish
 * Notation) byte buffers for FFI transport. This module deserializes and evaluates
 * those buffers.
 *
 * RPN Byte Format (from dd_ffi.h):
 * Each instruction is: [tag:u8] [payload...]
 *
 * Tags and payloads:
 * - 0x01 VAR: [name_len:u16le] [name:u8*name_len] (no NUL terminator)
 * - 0x02 CONST_INT: [value:i64le] (8 bytes)
 * - 0x03 CONST_STR: [len:u16le] [data:u8*len] (no NUL terminator)
 * - 0x04 BOOL: [value:u8] (0=false, 1=true)
 * - 0x10-0x14 ARITH (ADD,SUB,MUL,DIV,MOD): no payload, binary (pop 2, push 1)
 * - 0x20-0x25 CMP (EQ,NEQ,LT,GT,LTE,GTE): no payload, binary (pop 2, push 1)
 * - 0x30-0x33 AGG (COUNT,SUM,MIN,MAX): no payload, unary (pop 1, push 1)
 *
 * Evaluation: walk buffer left-to-right, push values onto stack, operators pop
 * operands and push result. Well-formed expression leaves exactly one value on stack.
 */

use crate::ffi_types::WlFfiExprTag;
use std::collections::HashMap;

/* ======================================================================== */
/* Decoded Expression Operations                                            */
/* ======================================================================== */

/// Decoded RPN instruction.
#[derive(Debug, Clone, PartialEq)]
pub enum ExprOp {
    Var(String),
    ConstInt(i64),
    ConstStr(String),
    Bool(bool),
    ArithAdd,
    ArithSub,
    ArithMul,
    ArithDiv,
    ArithMod,
    CmpEq,
    CmpNeq,
    CmpLt,
    CmpGt,
    CmpLte,
    CmpGte,
    AggCount,
    AggSum,
    AggMin,
    AggMax,
}

/* ======================================================================== */
/* Runtime Values                                                           */
/* ======================================================================== */

/// Runtime value on the evaluation stack.
#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    Int(i64),
    Str(String),
    Bool(bool),
}

/* ======================================================================== */
/* Error Types                                                              */
/* ======================================================================== */

/// Expression evaluation errors.
#[derive(Debug, PartialEq)]
pub enum ExprError {
    TruncatedBuffer,
    UnknownTag(u8),
    InvalidUtf8,
    StackUnderflow,
    TypeMismatch,
    UndefinedVariable(String),
    InvalidResult,
    DivisionByZero,
}

impl std::fmt::Display for ExprError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ExprError::TruncatedBuffer => write!(f, "Truncated expression buffer"),
            ExprError::UnknownTag(tag) => write!(f, "Unknown expression tag: 0x{:02x}", tag),
            ExprError::InvalidUtf8 => write!(f, "Invalid UTF-8 in expression string"),
            ExprError::StackUnderflow => write!(f, "Stack underflow during evaluation"),
            ExprError::TypeMismatch => write!(f, "Type mismatch in operation"),
            ExprError::UndefinedVariable(name) => write!(f, "Undefined variable: {}", name),
            ExprError::InvalidResult => write!(f, "Invalid expression result"),
            ExprError::DivisionByZero => write!(f, "Division by zero"),
        }
    }
}

impl std::error::Error for ExprError {}

/* ======================================================================== */
/* Deserialization                                                          */
/* ======================================================================== */

/// Deserialize an RPN expression from a byte buffer.
///
/// Returns a list of decoded ExprOp instructions.
///
/// # Errors
///
/// - `TruncatedBuffer`: Buffer ends in the middle of an instruction
/// - `UnknownTag`: Encountered an unrecognized tag byte
/// - `InvalidUtf8`: String payload contains invalid UTF-8
pub fn deserialize_expr(data: &[u8]) -> Result<Vec<ExprOp>, ExprError> {
    let mut ops = Vec::new();
    let mut pos = 0;

    while pos < data.len() {
        // Read tag byte
        let tag_byte = data[pos];
        pos += 1;

        let tag = WlFfiExprTag::from_byte(tag_byte).ok_or(ExprError::UnknownTag(tag_byte))?;

        match tag {
            WlFfiExprTag::Var => {
                // VAR: [name_len:u16le] [name:u8*name_len]
                if pos + 2 > data.len() {
                    return Err(ExprError::TruncatedBuffer);
                }
                let name_len = u16::from_le_bytes([data[pos], data[pos + 1]]) as usize;
                pos += 2;

                if pos + name_len > data.len() {
                    return Err(ExprError::TruncatedBuffer);
                }
                let name_bytes = &data[pos..pos + name_len];
                let name = std::str::from_utf8(name_bytes)
                    .map_err(|_| ExprError::InvalidUtf8)?
                    .to_string();
                pos += name_len;

                ops.push(ExprOp::Var(name));
            }

            WlFfiExprTag::ConstInt => {
                // CONST_INT: [value:i64le]
                if pos + 8 > data.len() {
                    return Err(ExprError::TruncatedBuffer);
                }
                let mut bytes = [0u8; 8];
                bytes.copy_from_slice(&data[pos..pos + 8]);
                let value = i64::from_le_bytes(bytes);
                pos += 8;

                ops.push(ExprOp::ConstInt(value));
            }

            WlFfiExprTag::ConstStr => {
                // CONST_STR: [len:u16le] [data:u8*len]
                if pos + 2 > data.len() {
                    return Err(ExprError::TruncatedBuffer);
                }
                let str_len = u16::from_le_bytes([data[pos], data[pos + 1]]) as usize;
                pos += 2;

                if pos + str_len > data.len() {
                    return Err(ExprError::TruncatedBuffer);
                }
                let str_bytes = &data[pos..pos + str_len];
                let string = std::str::from_utf8(str_bytes)
                    .map_err(|_| ExprError::InvalidUtf8)?
                    .to_string();
                pos += str_len;

                ops.push(ExprOp::ConstStr(string));
            }

            WlFfiExprTag::Bool => {
                // BOOL: [value:u8]
                if pos >= data.len() {
                    return Err(ExprError::TruncatedBuffer);
                }
                let value = data[pos] != 0;
                pos += 1;

                ops.push(ExprOp::Bool(value));
            }

            // Arithmetic operators (no payload)
            WlFfiExprTag::ArithAdd => ops.push(ExprOp::ArithAdd),
            WlFfiExprTag::ArithSub => ops.push(ExprOp::ArithSub),
            WlFfiExprTag::ArithMul => ops.push(ExprOp::ArithMul),
            WlFfiExprTag::ArithDiv => ops.push(ExprOp::ArithDiv),
            WlFfiExprTag::ArithMod => ops.push(ExprOp::ArithMod),

            // Comparison operators (no payload)
            WlFfiExprTag::CmpEq => ops.push(ExprOp::CmpEq),
            WlFfiExprTag::CmpNeq => ops.push(ExprOp::CmpNeq),
            WlFfiExprTag::CmpLt => ops.push(ExprOp::CmpLt),
            WlFfiExprTag::CmpGt => ops.push(ExprOp::CmpGt),
            WlFfiExprTag::CmpLte => ops.push(ExprOp::CmpLte),
            WlFfiExprTag::CmpGte => ops.push(ExprOp::CmpGte),

            // Aggregation operators (no payload)
            WlFfiExprTag::AggCount => ops.push(ExprOp::AggCount),
            WlFfiExprTag::AggSum => ops.push(ExprOp::AggSum),
            WlFfiExprTag::AggMin => ops.push(ExprOp::AggMin),
            WlFfiExprTag::AggMax => ops.push(ExprOp::AggMax),
        }
    }

    Ok(ops)
}

/* ======================================================================== */
/* Evaluation                                                               */
/* ======================================================================== */

/// Evaluate a filter expression (must return a boolean result).
///
/// # Arguments
///
/// - `ops`: Sequence of RPN instructions
/// - `vars`: Variable bindings (variable name -> value)
///
/// # Returns
///
/// The boolean result of the filter expression.
///
/// # Errors
///
/// - `StackUnderflow`: Operator tried to pop from empty stack
/// - `TypeMismatch`: Operation applied to incompatible types
/// - `UndefinedVariable`: Variable not found in bindings
/// - `InvalidResult`: Expression did not leave exactly one Bool on stack
/// - `DivisionByZero`: Attempted division or modulo by zero
pub fn eval_filter(ops: &[ExprOp], vars: &HashMap<String, Value>) -> Result<bool, ExprError> {
    let mut stack: Vec<Value> = Vec::new();

    for op in ops {
        match op {
            ExprOp::Var(name) => {
                let value = vars
                    .get(name)
                    .ok_or_else(|| ExprError::UndefinedVariable(name.clone()))?;
                stack.push(value.clone());
            }

            ExprOp::ConstInt(val) => {
                stack.push(Value::Int(*val));
            }

            ExprOp::ConstStr(s) => {
                stack.push(Value::Str(s.clone()));
            }

            ExprOp::Bool(b) => {
                stack.push(Value::Bool(*b));
            }

            // Arithmetic operations (binary, require Int operands)
            ExprOp::ArithAdd => {
                let b = pop_int(&mut stack)?;
                let a = pop_int(&mut stack)?;
                stack.push(Value::Int(a + b));
            }

            ExprOp::ArithSub => {
                let b = pop_int(&mut stack)?;
                let a = pop_int(&mut stack)?;
                stack.push(Value::Int(a - b));
            }

            ExprOp::ArithMul => {
                let b = pop_int(&mut stack)?;
                let a = pop_int(&mut stack)?;
                stack.push(Value::Int(a * b));
            }

            ExprOp::ArithDiv => {
                let b = pop_int(&mut stack)?;
                if b == 0 {
                    return Err(ExprError::DivisionByZero);
                }
                let a = pop_int(&mut stack)?;
                stack.push(Value::Int(a / b));
            }

            ExprOp::ArithMod => {
                let b = pop_int(&mut stack)?;
                if b == 0 {
                    return Err(ExprError::DivisionByZero);
                }
                let a = pop_int(&mut stack)?;
                stack.push(Value::Int(a % b));
            }

            // Comparison operations (binary, return Bool)
            ExprOp::CmpEq => {
                let b = pop_value(&mut stack)?;
                let a = pop_value(&mut stack)?;
                stack.push(Value::Bool(a == b));
            }

            ExprOp::CmpNeq => {
                let b = pop_value(&mut stack)?;
                let a = pop_value(&mut stack)?;
                stack.push(Value::Bool(a != b));
            }

            ExprOp::CmpLt => {
                let b = pop_value(&mut stack)?;
                let a = pop_value(&mut stack)?;
                let result = compare_values(&a, &b)?;
                stack.push(Value::Bool(result < 0));
            }

            ExprOp::CmpGt => {
                let b = pop_value(&mut stack)?;
                let a = pop_value(&mut stack)?;
                let result = compare_values(&a, &b)?;
                stack.push(Value::Bool(result > 0));
            }

            ExprOp::CmpLte => {
                let b = pop_value(&mut stack)?;
                let a = pop_value(&mut stack)?;
                let result = compare_values(&a, &b)?;
                stack.push(Value::Bool(result <= 0));
            }

            ExprOp::CmpGte => {
                let b = pop_value(&mut stack)?;
                let a = pop_value(&mut stack)?;
                let result = compare_values(&a, &b)?;
                stack.push(Value::Bool(result >= 0));
            }

            // Aggregation operations (pass through for now, used in REDUCE not FILTER)
            ExprOp::AggCount | ExprOp::AggSum | ExprOp::AggMin | ExprOp::AggMax => {
                // These are not evaluated in FILTER context; they're used in REDUCE.
                // For now, we just pass through whatever is on the stack.
                // In a real implementation, these would be handled differently.
                let _val = pop_value(&mut stack)?;
                stack.push(_val);
            }
        }
    }

    // Final stack must have exactly one Bool value
    if stack.len() != 1 {
        return Err(ExprError::InvalidResult);
    }

    match stack.pop().unwrap() {
        Value::Bool(b) => Ok(b),
        _ => Err(ExprError::InvalidResult),
    }
}

/* ======================================================================== */
/* Helper Functions                                                         */
/* ======================================================================== */

/// Pop a value from the stack, return error if empty.
fn pop_value(stack: &mut Vec<Value>) -> Result<Value, ExprError> {
    stack.pop().ok_or(ExprError::StackUnderflow)
}

/// Pop an Int value from the stack.
fn pop_int(stack: &mut Vec<Value>) -> Result<i64, ExprError> {
    match pop_value(stack)? {
        Value::Int(i) => Ok(i),
        _ => Err(ExprError::TypeMismatch),
    }
}

/// Compare two values, returning -1, 0, or 1.
/// Values must be of the same type.
fn compare_values(a: &Value, b: &Value) -> Result<i32, ExprError> {
    match (a, b) {
        (Value::Int(x), Value::Int(y)) => Ok(if x < y {
            -1
        } else if x > y {
            1
        } else {
            0
        }),
        (Value::Str(x), Value::Str(y)) => Ok(if x < y {
            -1
        } else if x > y {
            1
        } else {
            0
        }),
        (Value::Bool(x), Value::Bool(y)) => Ok(if x < y {
            -1
        } else if x > y {
            1
        } else {
            0
        }),
        _ => Err(ExprError::TypeMismatch),
    }
}

/* ======================================================================== */
/* Tests                                                                    */
/* ======================================================================== */

#[cfg(test)]
mod tests {
    use super::*;

    // ---- Deserialization Tests ----

    #[test]
    fn test_deserialize_var() {
        // VAR "X": [0x01] [0x01, 0x00] [0x58]
        let data = vec![0x01, 0x01, 0x00, 0x58];
        let ops = deserialize_expr(&data).unwrap();
        assert_eq!(ops, vec![ExprOp::Var("X".to_string())]);
    }

    #[test]
    fn test_deserialize_const_int() {
        // CONST_INT 42: [0x02] [42, 0, 0, 0, 0, 0, 0, 0]
        let data = vec![0x02, 42, 0, 0, 0, 0, 0, 0, 0];
        let ops = deserialize_expr(&data).unwrap();
        assert_eq!(ops, vec![ExprOp::ConstInt(42)]);
    }

    #[test]
    fn test_deserialize_const_int_negative() {
        // CONST_INT -10: [0x02] [246, 255, 255, 255, 255, 255, 255, 255]
        let value: i64 = -10;
        let mut data = vec![0x02];
        data.extend_from_slice(&value.to_le_bytes());
        let ops = deserialize_expr(&data).unwrap();
        assert_eq!(ops, vec![ExprOp::ConstInt(-10)]);
    }

    #[test]
    fn test_deserialize_const_str() {
        // CONST_STR "hello": [0x03] [0x05, 0x00] [h, e, l, l, o]
        let data = vec![0x03, 0x05, 0x00, b'h', b'e', b'l', b'l', b'o'];
        let ops = deserialize_expr(&data).unwrap();
        assert_eq!(ops, vec![ExprOp::ConstStr("hello".to_string())]);
    }

    #[test]
    fn test_deserialize_bool_true() {
        // BOOL true: [0x04] [0x01]
        let data = vec![0x04, 0x01];
        let ops = deserialize_expr(&data).unwrap();
        assert_eq!(ops, vec![ExprOp::Bool(true)]);
    }

    #[test]
    fn test_deserialize_bool_false() {
        // BOOL false: [0x04] [0x00]
        let data = vec![0x04, 0x00];
        let ops = deserialize_expr(&data).unwrap();
        assert_eq!(ops, vec![ExprOp::Bool(false)]);
    }

    #[test]
    fn test_deserialize_compound() {
        // X > 5: VAR "X", CONST_INT 5, CMP_GT
        // [0x01] [0x01, 0x00] [0x58] [0x02] [5, 0, 0, 0, 0, 0, 0, 0] [0x23]
        let mut data = vec![0x01, 0x01, 0x00, 0x58]; // VAR "X"
        data.push(0x02); // CONST_INT
        data.extend_from_slice(&5i64.to_le_bytes());
        data.push(0x23); // CMP_GT

        let ops = deserialize_expr(&data).unwrap();
        assert_eq!(
            ops,
            vec![
                ExprOp::Var("X".to_string()),
                ExprOp::ConstInt(5),
                ExprOp::CmpGt
            ]
        );
    }

    #[test]
    fn test_deserialize_truncated() {
        // VAR with incomplete name length
        let data = vec![0x01, 0x05];
        let result = deserialize_expr(&data);
        assert_eq!(result, Err(ExprError::TruncatedBuffer));

        // CONST_INT with incomplete value
        let data = vec![0x02, 0x01, 0x02];
        let result = deserialize_expr(&data);
        assert_eq!(result, Err(ExprError::TruncatedBuffer));

        // CONST_STR with incomplete data
        let data = vec![0x03, 0x05, 0x00, b'h', b'i'];
        let result = deserialize_expr(&data);
        assert_eq!(result, Err(ExprError::TruncatedBuffer));
    }

    #[test]
    fn test_deserialize_unknown_tag() {
        let data = vec![0xFF];
        let result = deserialize_expr(&data);
        assert_eq!(result, Err(ExprError::UnknownTag(0xFF)));

        let data = vec![0x05];
        let result = deserialize_expr(&data);
        assert_eq!(result, Err(ExprError::UnknownTag(0x05)));
    }

    // ---- Evaluation Tests ----

    #[test]
    fn test_eval_simple_gt() {
        // X > 5 with X = 10
        let ops = vec![
            ExprOp::Var("X".to_string()),
            ExprOp::ConstInt(5),
            ExprOp::CmpGt,
        ];
        let mut vars = HashMap::new();
        vars.insert("X".to_string(), Value::Int(10));

        let result = eval_filter(&ops, &vars).unwrap();
        assert_eq!(result, true);
    }

    #[test]
    fn test_eval_simple_gt_false() {
        // X > 5 with X = 3
        let ops = vec![
            ExprOp::Var("X".to_string()),
            ExprOp::ConstInt(5),
            ExprOp::CmpGt,
        ];
        let mut vars = HashMap::new();
        vars.insert("X".to_string(), Value::Int(3));

        let result = eval_filter(&ops, &vars).unwrap();
        assert_eq!(result, false);
    }

    #[test]
    fn test_eval_arithmetic() {
        // (A + B) > 10 with A = 7, B = 5
        // RPN: A B + 10 >
        let ops = vec![
            ExprOp::Var("A".to_string()),
            ExprOp::Var("B".to_string()),
            ExprOp::ArithAdd,
            ExprOp::ConstInt(10),
            ExprOp::CmpGt,
        ];
        let mut vars = HashMap::new();
        vars.insert("A".to_string(), Value::Int(7));
        vars.insert("B".to_string(), Value::Int(5));

        let result = eval_filter(&ops, &vars).unwrap();
        assert_eq!(result, true); // 7 + 5 = 12 > 10
    }

    #[test]
    fn test_eval_arithmetic_complex() {
        // ((A + B) * 2) >= 20 with A = 3, B = 7
        // RPN: A B + 2 * 20 >=
        let ops = vec![
            ExprOp::Var("A".to_string()),
            ExprOp::Var("B".to_string()),
            ExprOp::ArithAdd,
            ExprOp::ConstInt(2),
            ExprOp::ArithMul,
            ExprOp::ConstInt(20),
            ExprOp::CmpGte,
        ];
        let mut vars = HashMap::new();
        vars.insert("A".to_string(), Value::Int(3));
        vars.insert("B".to_string(), Value::Int(7));

        let result = eval_filter(&ops, &vars).unwrap();
        assert_eq!(result, true); // (3 + 7) * 2 = 20 >= 20
    }

    #[test]
    fn test_eval_string_eq() {
        // name = "alice"
        let ops = vec![
            ExprOp::Var("name".to_string()),
            ExprOp::ConstStr("alice".to_string()),
            ExprOp::CmpEq,
        ];
        let mut vars = HashMap::new();
        vars.insert("name".to_string(), Value::Str("alice".to_string()));

        let result = eval_filter(&ops, &vars).unwrap();
        assert_eq!(result, true);
    }

    #[test]
    fn test_eval_string_eq_false() {
        // name = "alice" with name = "bob"
        let ops = vec![
            ExprOp::Var("name".to_string()),
            ExprOp::ConstStr("alice".to_string()),
            ExprOp::CmpEq,
        ];
        let mut vars = HashMap::new();
        vars.insert("name".to_string(), Value::Str("bob".to_string()));

        let result = eval_filter(&ops, &vars).unwrap();
        assert_eq!(result, false);
    }

    #[test]
    fn test_eval_string_comparison() {
        // name < "charlie" with name = "alice"
        let ops = vec![
            ExprOp::Var("name".to_string()),
            ExprOp::ConstStr("charlie".to_string()),
            ExprOp::CmpLt,
        ];
        let mut vars = HashMap::new();
        vars.insert("name".to_string(), Value::Str("alice".to_string()));

        let result = eval_filter(&ops, &vars).unwrap();
        assert_eq!(result, true); // "alice" < "charlie"
    }

    #[test]
    fn test_eval_undefined_var() {
        // X > 5 with no X defined
        let ops = vec![
            ExprOp::Var("X".to_string()),
            ExprOp::ConstInt(5),
            ExprOp::CmpGt,
        ];
        let vars = HashMap::new();

        let result = eval_filter(&ops, &vars);
        assert_eq!(result, Err(ExprError::UndefinedVariable("X".to_string())));
    }

    #[test]
    fn test_eval_type_mismatch() {
        // X > "hello" with X = 5 (comparing int with string)
        let ops = vec![
            ExprOp::Var("X".to_string()),
            ExprOp::ConstStr("hello".to_string()),
            ExprOp::CmpGt,
        ];
        let mut vars = HashMap::new();
        vars.insert("X".to_string(), Value::Int(5));

        let result = eval_filter(&ops, &vars);
        assert_eq!(result, Err(ExprError::TypeMismatch));
    }

    #[test]
    fn test_eval_type_mismatch_arithmetic() {
        // X + "hello" with X = 5
        let ops = vec![
            ExprOp::Var("X".to_string()),
            ExprOp::ConstStr("hello".to_string()),
            ExprOp::ArithAdd,
        ];
        let mut vars = HashMap::new();
        vars.insert("X".to_string(), Value::Int(5));

        let result = eval_filter(&ops, &vars);
        assert_eq!(result, Err(ExprError::TypeMismatch));
    }

    #[test]
    fn test_eval_division_by_zero() {
        // X / 0
        let ops = vec![
            ExprOp::Var("X".to_string()),
            ExprOp::ConstInt(0),
            ExprOp::ArithDiv,
        ];
        let mut vars = HashMap::new();
        vars.insert("X".to_string(), Value::Int(10));

        let result = eval_filter(&ops, &vars);
        assert_eq!(result, Err(ExprError::DivisionByZero));
    }

    #[test]
    fn test_eval_modulo_by_zero() {
        // X % 0
        let ops = vec![
            ExprOp::Var("X".to_string()),
            ExprOp::ConstInt(0),
            ExprOp::ArithMod,
        ];
        let mut vars = HashMap::new();
        vars.insert("X".to_string(), Value::Int(10));

        let result = eval_filter(&ops, &vars);
        assert_eq!(result, Err(ExprError::DivisionByZero));
    }

    #[test]
    fn test_eval_empty_expr() {
        let ops = vec![];
        let vars = HashMap::new();

        let result = eval_filter(&ops, &vars);
        assert_eq!(result, Err(ExprError::InvalidResult));
    }

    #[test]
    fn test_eval_invalid_result_too_many() {
        // Push two values but no operator (stack has 2 items at end)
        let ops = vec![ExprOp::ConstInt(5), ExprOp::ConstInt(10)];
        let vars = HashMap::new();

        let result = eval_filter(&ops, &vars);
        assert_eq!(result, Err(ExprError::InvalidResult));
    }

    #[test]
    fn test_eval_invalid_result_not_bool() {
        // Expression that leaves an Int on stack instead of Bool
        let ops = vec![ExprOp::ConstInt(5)];
        let vars = HashMap::new();

        let result = eval_filter(&ops, &vars);
        assert_eq!(result, Err(ExprError::InvalidResult));
    }

    #[test]
    fn test_eval_stack_underflow() {
        // Try to add without enough operands
        let ops = vec![ExprOp::ConstInt(5), ExprOp::ArithAdd];
        let vars = HashMap::new();

        let result = eval_filter(&ops, &vars);
        assert_eq!(result, Err(ExprError::StackUnderflow));
    }

    #[test]
    fn test_eval_neq() {
        // X != 5 with X = 10
        let ops = vec![
            ExprOp::Var("X".to_string()),
            ExprOp::ConstInt(5),
            ExprOp::CmpNeq,
        ];
        let mut vars = HashMap::new();
        vars.insert("X".to_string(), Value::Int(10));

        let result = eval_filter(&ops, &vars).unwrap();
        assert_eq!(result, true);
    }

    #[test]
    fn test_eval_lte() {
        // X <= 5 with X = 5
        let ops = vec![
            ExprOp::Var("X".to_string()),
            ExprOp::ConstInt(5),
            ExprOp::CmpLte,
        ];
        let mut vars = HashMap::new();
        vars.insert("X".to_string(), Value::Int(5));

        let result = eval_filter(&ops, &vars).unwrap();
        assert_eq!(result, true);
    }

    #[test]
    fn test_eval_subtraction() {
        // (10 - X) > 3 with X = 5
        let ops = vec![
            ExprOp::ConstInt(10),
            ExprOp::Var("X".to_string()),
            ExprOp::ArithSub,
            ExprOp::ConstInt(3),
            ExprOp::CmpGt,
        ];
        let mut vars = HashMap::new();
        vars.insert("X".to_string(), Value::Int(5));

        let result = eval_filter(&ops, &vars).unwrap();
        assert_eq!(result, true); // 10 - 5 = 5 > 3
    }

    #[test]
    fn test_eval_modulo() {
        // X % 3 = 1 with X = 10
        let ops = vec![
            ExprOp::Var("X".to_string()),
            ExprOp::ConstInt(3),
            ExprOp::ArithMod,
            ExprOp::ConstInt(1),
            ExprOp::CmpEq,
        ];
        let mut vars = HashMap::new();
        vars.insert("X".to_string(), Value::Int(10));

        let result = eval_filter(&ops, &vars).unwrap();
        assert_eq!(result, true); // 10 % 3 = 1
    }

    #[test]
    fn test_eval_bool_comparison() {
        // flag = true
        let ops = vec![
            ExprOp::Var("flag".to_string()),
            ExprOp::Bool(true),
            ExprOp::CmpEq,
        ];
        let mut vars = HashMap::new();
        vars.insert("flag".to_string(), Value::Bool(true));

        let result = eval_filter(&ops, &vars).unwrap();
        assert_eq!(result, true);
    }

    // ---- Integration Tests (Deserialize + Eval) ----

    #[test]
    fn test_integration_simple_filter() {
        // Build: X > 5
        let mut data = vec![0x01, 0x01, 0x00, 0x58]; // VAR "X"
        data.push(0x02); // CONST_INT
        data.extend_from_slice(&5i64.to_le_bytes());
        data.push(0x23); // CMP_GT

        let ops = deserialize_expr(&data).unwrap();
        let mut vars = HashMap::new();
        vars.insert("X".to_string(), Value::Int(10));

        let result = eval_filter(&ops, &vars).unwrap();
        assert_eq!(result, true);
    }

    #[test]
    fn test_integration_arithmetic_filter() {
        // Build: (A + B) >= 10 with A=3, B=8
        let mut data = vec![];
        // VAR "A"
        data.push(0x01);
        data.extend_from_slice(&1u16.to_le_bytes());
        data.push(b'A');
        // VAR "B"
        data.push(0x01);
        data.extend_from_slice(&1u16.to_le_bytes());
        data.push(b'B');
        // ArithAdd
        data.push(0x10);
        // CONST_INT 10
        data.push(0x02);
        data.extend_from_slice(&10i64.to_le_bytes());
        // CmpGte
        data.push(0x25);

        let ops = deserialize_expr(&data).unwrap();
        let mut vars = HashMap::new();
        vars.insert("A".to_string(), Value::Int(3));
        vars.insert("B".to_string(), Value::Int(8));

        let result = eval_filter(&ops, &vars).unwrap();
        assert_eq!(result, true); // 3 + 8 = 11 >= 10
    }
}
