if [[ ! ("$TRAVIS_COMPILER" == "clang") && ("$CONFIG" == "debug") ]]; then
  bash <(curl -s https://codecov.io/bash)
else
  echo "No coverage data in this configuration"
fi
exit 0

