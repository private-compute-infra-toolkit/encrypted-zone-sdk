// Copyright 2025 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//	http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// go implementation running protoc using protoc_gen_doc.
package main

import (
	"fmt"
	"io"
	"io/fs"
	"io/ioutil"
	"log"
	"os"
	"path"

	"github.com/pseudomuto/protoc-gen-doc"
	"github.com/golang/protobuf/proto"

	pluginpb "github.com/golang/protobuf/protoc-gen-go/plugin"
)

var (
	tmplWorkdir string
	// templateDirName is defined in gen_template_dir genrule.
	tmplSymlink string = templateDirName
)

func copyToDir(srcFS fs.FS, destDir string) error {
	return fs.WalkDir(srcFS, ".", func(filePath string, d fs.DirEntry, e error) error {
		if e != nil {
			return e
		}
		if d.IsDir() {
			return e
		}
		if !d.Type().IsDir() {
			_destDir := path.Join(destDir, path.Dir(filePath))
			if dirErr := os.MkdirAll(_destDir, 0755); dirErr != nil {
				return dirErr
			}
			_, err := copyFile(srcFS, filePath, path.Join(_destDir, d.Name()))
			if err != nil {
				return err
			}
		}
		return nil
	})
}

func copyFile(srcFS fs.FS, srcPath string, destPath string) (int64, error) {
	source, err := srcFS.Open(srcPath)
	if err != nil {
		return 0, err
	}
	defer source.Close()
	destination, err := os.Create(destPath)
	if err != nil {
		return 0, err
	}
	sourceFileInfo, err := source.Stat()
	if err != nil {
		return 0, err
	}
	if !sourceFileInfo.Mode().IsRegular() {
		return 0, nil
	}
	if err = destination.Chmod(sourceFileInfo.Mode()); err != nil {
		return 0, err
	}
	defer destination.Close()
	return io.Copy(destination, source)
}

func listFS(srcFS fs.FS) error {
	return fs.WalkDir(srcFS, ".", func(path string, d fs.DirEntry, e error) error {
		if e != nil {
			return e
		}
		if !d.Type().IsDir() {
			if info, e := d.Info(); e != nil {
				fmt.Printf("%s [unknown]\n", path)
				return e
			} else {
				fmt.Printf("%s [%d]\n", path, info.Size())
			}
		}
		return nil
	})
}

func extractTemplates() (err error) {
	tmplPath := os.Getenv("TEMPLATE_PATH")

	// tmplWorkdir is a temp dir into which template files are copied
	if tmplWorkdir, err = os.MkdirTemp("", "protocgendoctmpl"); err != nil {
		return
	}

	// Create the symlink expected by protoc-gen-doc
	if err = os.Symlink(tmplWorkdir, tmplSymlink); os.IsExist(err) {
		if err = os.Remove(tmplSymlink); err != nil {
			err = fmt.Errorf("unable to remove symlink: %s", tmplSymlink)
			return
		}
		if err = os.Symlink(tmplWorkdir, tmplSymlink); err != nil {
			err = fmt.Errorf("unable to create symlink: %s", tmplSymlink)
			return
		}
	} else if err != nil {
		return
	}

	templateFS := os.DirFS(tmplPath)
	if err = copyToDir(templateFS, tmplWorkdir); err != nil {
		return fmt.Errorf("failed to copy templates: %w", err)
	}

	return
}

// cleanup removes the temporary symlink created during template extraction.
func cleanup() (err error) {
	err = os.Remove(tmplSymlink)
	return
}

// protoc-gen-doc is used to generate documentation from comments in your proto files.
//
// It is a protoc plugin, and can be invoked by passing `--doc_out` and `--doc_opt` arguments to protoc.
//
// Example: generate HTML documentation
//
//     protoc --doc_out=. --doc_opt=html,index.html protos/*.proto
//
// Example: use a custom template
//
//     protoc --doc_out=. --doc_opt=custom.tmpl,docs.txt protos/*.proto
//
// For more details, check out the README at https://github.com/pseudomuto/protoc-gen-doc

func main() {
	input, err := ioutil.ReadAll(os.Stdin)
	if err != nil {
		log.Fatalf("Could not read contents from stdin")
	}

	req := new(pluginpb.CodeGeneratorRequest)
	if err = proto.Unmarshal(input, req); err != nil {
		log.Fatal(err)
	}

	if err := extractTemplates(); err != nil {
		log.Fatal(err)
	}
	defer os.RemoveAll(tmplWorkdir)
	defer cleanup()

	p := new(gendoc.Plugin)
	resp, err := p.Generate(req)

	if err != nil {
		log.Fatal(err)
	}

	data, err := proto.Marshal(resp)
	if err != nil {
		log.Fatal(err)
	}

	if _, err := os.Stdout.Write(data); err != nil {
		log.Fatal(err)
	}
}
