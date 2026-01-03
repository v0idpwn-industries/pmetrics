.PHONY: build clean test run docker-build docker-run all

# Binary name
BINARY_NAME=pmetrics-exporter

# Build the binary
build:
	go build -o $(BINARY_NAME) .

# Build static binary (for Linux deployment)
build-static:
	CGO_ENABLED=0 go build -a -ldflags '-s -w -extldflags "-static"' -o $(BINARY_NAME) .

# Build for multiple platforms
build-all:
	@echo "Building for multiple platforms..."
	CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -o $(BINARY_NAME)-linux-amd64 .
	CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -o $(BINARY_NAME)-linux-arm64 .
	CGO_ENABLED=0 GOOS=darwin GOARCH=amd64 go build -o $(BINARY_NAME)-darwin-amd64 .
	CGO_ENABLED=0 GOOS=darwin GOARCH=arm64 go build -o $(BINARY_NAME)-darwin-arm64 .
	CGO_ENABLED=0 GOOS=windows GOARCH=amd64 go build -o $(BINARY_NAME)-windows-amd64.exe .
	@echo "Done building all platforms"

# Clean build artifacts
clean:
	go clean
	rm -f $(BINARY_NAME)
	rm -f $(BINARY_NAME)-*

# Run tests
test:
	go test -v ./...

# Run the exporter (requires DATABASE_URL env var)
run: build
	./$(BINARY_NAME)

# Format code
fmt:
	go fmt ./...

# Run linter (requires golangci-lint)
lint:
	golangci-lint run

# Build Docker image
docker-build:
	docker build -t pmetrics-exporter:latest .

# Run Docker container (requires DATABASE_URL env var)
docker-run:
	docker run -e DATABASE_URL=${DATABASE_URL} -p 9187:9187 pmetrics-exporter:latest

# Install dependencies
deps:
	go mod download
	go mod tidy

# Default target
all: clean build test
