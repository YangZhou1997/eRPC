#!/bin/bash

function force_cleanup {
  echo "force_cleanup"
  exit 1
}
trap force_cleanup SIGINT

nu_erpc=$(seq 2 2 5; seq 5 5 151)
./run_dint.sh binary store
for nu in ${nu_erpc[@]}; do
  timeout 600s ./run_dint.sh run store $nu 8 || true
  if [ $? -ne 124 ]; then
    echo "erpc store $nu 8 done"
  fi
done

nu_erpc=$(seq 2 2 5; seq 5 5 151)
./run_dint.sh binary lock_fasst
for nu in ${nu_erpc[@]}; do
  timeout 600s ./run_dint.sh run lock_fasst $nu 8 || true
  if [ $? -ne 124 ]; then
    echo "erpc lock_fasst $nu 8 done"
  fi
done
