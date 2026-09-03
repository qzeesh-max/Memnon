#!/bin/bash
set -e

# Build the docker image
echo "Building Docker image for Linux cross-testing..."
docker build -t segmented_interprocess_linux_test .

# Run the docker container
echo "Running tests in Docker container..."
docker run --rm -v $(pwd):/workspace_host segmented_interprocess_linux_test

echo "Docker tests completed successfully!"
