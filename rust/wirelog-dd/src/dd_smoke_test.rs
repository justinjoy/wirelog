/*
 * dd_smoke_test.rs - Verify DD API works with Vec<i64> data type
 *
 * Temporary smoke test to validate our DD integration approach.
 */

#[cfg(test)]
mod tests {
    use std::sync::{Arc, Mutex};

    use differential_dataflow::input::Input;
    use differential_dataflow::operators::{Iterate, Join, Reduce, Threshold};
    #[allow(unused_imports)]
    use timely::dataflow::operators::probe::Probe;

    type Row = Vec<i64>;

    #[test]
    fn test_dd_basic_map_filter() {
        let results: Arc<Mutex<Vec<Row>>> = Arc::new(Mutex::new(Vec::new()));
        let results_clone = results.clone();

        timely::execute(timely::Config::thread(), move |worker| {
            let probe = worker.dataflow::<(), _, _>(|scope| {
                let (_input, edges) =
                    scope.new_collection_from(vec![vec![1i64, 2], vec![2, 3], vec![3, 4]]);

                let filtered = edges
                    .filter(|row: &Row| row[0] > 1)
                    .map(|row: Row| vec![row[1], row[0]]); // swap columns

                let results_ref = results_clone.clone();
                filtered
                    .inspect(move |&(ref data, _time, diff): &(Row, (), isize)| {
                        if diff > 0 {
                            results_ref.lock().unwrap().push(data.clone());
                        }
                    })
                    .probe()
            });

            worker.step_while(|| !probe.done());
        })
        .unwrap();

        let mut got = results.lock().unwrap().clone();
        got.sort();
        assert_eq!(got, vec![vec![3, 2], vec![4, 3]]);
    }

    #[test]
    fn test_dd_join() {
        let results: Arc<Mutex<Vec<Row>>> = Arc::new(Mutex::new(Vec::new()));
        let results_clone = results.clone();

        timely::execute(timely::Config::thread(), move |worker| {
            let probe = worker.dataflow::<(), _, _>(|scope| {
                // a(X,Y): keyed by Y (last col), val = full row
                let (_, a) =
                    scope.new_collection_from(vec![(2i64, vec![1i64, 2]), (2i64, vec![3i64, 2])]);
                // b(Y,Z): keyed by Y (first col), val = non-key cols
                let (_, b) =
                    scope.new_collection_from(vec![(2i64, vec![5i64]), (2i64, vec![6i64])]);

                let joined = a.join_map(&b, |_key: &i64, lrow: &Row, rval: &Row| -> Row {
                    let mut out = lrow.clone();
                    out.extend(rval.iter());
                    out
                });

                let results_ref = results_clone.clone();
                joined
                    .inspect(move |&(ref data, _time, diff): &(Row, (), isize)| {
                        if diff > 0 {
                            results_ref.lock().unwrap().push(data.clone());
                        }
                    })
                    .probe()
            });

            worker.step_while(|| !probe.done());
        })
        .unwrap();

        let mut got = results.lock().unwrap().clone();
        got.sort();
        assert_eq!(got.len(), 4);
        assert!(got.contains(&vec![1, 2, 5]));
        assert!(got.contains(&vec![1, 2, 6]));
        assert!(got.contains(&vec![3, 2, 5]));
        assert!(got.contains(&vec![3, 2, 6]));
    }

    #[test]
    fn test_dd_transitive_closure() {
        let results: Arc<Mutex<Vec<Row>>> = Arc::new(Mutex::new(Vec::new()));
        let results_clone = results.clone();

        timely::execute(timely::Config::thread(), move |worker| {
            let probe = worker.dataflow::<(), _, _>(|scope| {
                // edge: 1->2, 2->3, 3->4
                let (_, edges) =
                    scope.new_collection_from(vec![vec![1i64, 2], vec![2, 3], vec![3, 4]]);

                // TC: base case + recursive join via iterate()
                let tc = edges.iterate(|inner| {
                    let edges_entered = edges.enter(&inner.scope());

                    // inner is tc so far (as Vec<i64> rows [X, Y])
                    // join tc(X,Y) with edge(Y,Z) on tc.col1 == edge.col0
                    let tc_keyed = inner.map(|row: Row| (row[1], row));
                    let edge_keyed = edges_entered.map(|row: Row| (row[0], vec![row[1]]));

                    let new_paths = tc_keyed
                        .join_map(&edge_keyed, |_y: &i64, tc_row: &Row, edge_val: &Row| {
                            vec![tc_row[0], edge_val[0]]
                        });

                    inner.concat(&new_paths).distinct()
                });

                let results_ref = results_clone.clone();
                tc.inspect(move |&(ref data, _time, ref _diff): &(Row, _, isize)| {
                    results_ref.lock().unwrap().push(data.clone());
                })
                .probe()
            });

            worker.step_while(|| !probe.done());
        })
        .unwrap();

        let mut got = results.lock().unwrap().clone();
        got.sort();
        got.dedup();
        // Expected: (1,2), (1,3), (1,4), (2,3), (2,4), (3,4)
        assert_eq!(got.len(), 6);
        assert!(got.contains(&vec![1, 2]));
        assert!(got.contains(&vec![1, 3]));
        assert!(got.contains(&vec![1, 4]));
        assert!(got.contains(&vec![2, 3]));
        assert!(got.contains(&vec![2, 4]));
        assert!(got.contains(&vec![3, 4]));
    }

    #[test]
    fn test_dd_reduce_count() {
        let results: Arc<Mutex<Vec<(Row, i64)>>> = Arc::new(Mutex::new(Vec::new()));
        let results_clone = results.clone();

        timely::execute(timely::Config::thread(), move |worker| {
            let probe = worker.dataflow::<(), _, _>(|scope| {
                // data: (group, value) = (1,10), (1,20), (2,30)
                // count by group -> (1, 2), (2, 1)
                let (_, data) =
                    scope.new_collection_from(vec![(1i64, 10i64), (1i64, 20i64), (2i64, 30i64)]);

                let counted = data.reduce(
                    |_key: &i64, input: &[(&i64, isize)], output: &mut Vec<(i64, isize)>| {
                        let count: isize = input.iter().map(|(_, c)| c).sum();
                        output.push((count as i64, 1));
                    },
                );

                let results_ref = results_clone.clone();
                counted
                    .inspect(move |&(ref data, _time, diff): &((i64, i64), (), isize)| {
                        if diff > 0 {
                            results_ref.lock().unwrap().push((vec![data.0], data.1));
                        }
                    })
                    .probe()
            });

            worker.step_while(|| !probe.done());
        })
        .unwrap();

        let got = results.lock().unwrap().clone();
        assert_eq!(got.len(), 2);
        // group 1: count 2, group 2: count 1
        assert!(got.contains(&(vec![1], 2)));
        assert!(got.contains(&(vec![2], 1)));
    }
}
