package main

import (
	"embed"
	"net/http"
)

//go:embed web/*
var webFiles embed.FS

func embeddedWebFile(name string) http.Handler {
	return http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		http.ServeFileFS(writer, request, webFiles, "web/"+name)
	})
}
