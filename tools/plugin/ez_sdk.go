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
	gendoc "github.com/pseudomuto/protoc-gen-doc/extensions"

	ezsdkpb "github.com/privacysandbox/ez/sdk/v1"
)

func init() {
	gendoc.SetTransformer(
		"enforcer.v1.ez_svc_annotation",
		func(payload interface{}) interface{} {
			if obj, ok := payload.(*ezsdkpb.EzServiceAnnotation); ok {
				return obj
			} else {
				return nil
			}
		},
	)
	gendoc.SetTransformer(
		"enforcer.v1.ez_rpc_annotation",
		func(payload interface{}) interface{} {
			if obj, ok := payload.(*ezsdkpb.EzFunctionAnnotation); ok {
				return obj
			} else {
				return nil
			}
		},
	)
	gendoc.SetTransformer(
		"enforcer.v1.ez_msg_annotation",
		func(payload interface{}) interface{} {
			if obj, ok := payload.(*ezsdkpb.EzMessageAnnotation); ok {
				return obj
			} else {
				return nil
			}
		},
	)
	gendoc.SetTransformer(
		"enforcer.v1.ez_field_annotation",
		func(payload interface{}) interface{} {
			if obj, ok := payload.(*ezsdkpb.EzFieldAnnotation); ok {
				return obj
			} else {
				return nil
			}
		},
	)
}
