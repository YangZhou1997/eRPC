#!/bin/bash

function force_cleanup {
  echo "force_cleanup"
  exit 1
}
trap force_cleanup SIGINT

nu_erpc=$(seq 10 10 301)
for nu in ${nu_erpc[@]}; do
  timeout 600s ./run_dint.sh run store $nu || true
  if [ $? -ne 124 ]; then
    echo "erpc store $nu done"
  fi
done
