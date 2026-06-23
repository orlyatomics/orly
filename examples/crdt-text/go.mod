module github.com/orlyatomics/orly/examples/crdt-text

go 1.22

require github.com/orlyatomics/orly/clients/go v0.0.0

require github.com/gorilla/websocket v1.5.3 // indirect

replace github.com/orlyatomics/orly/clients/go => ../../clients/go
