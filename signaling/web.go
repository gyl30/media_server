package main

import (
	"embed"
	"io/fs"
	"net/http"
)

//go:embed web/*
var webFiles embed.FS

func embeddedWebHandler() http.Handler {
	root, err := fs.Sub(webFiles, "web")
	if err != nil {
		panic(err)
	}
	return http.FileServerFS(root)
}
