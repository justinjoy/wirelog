# libsoup 아키텍처 패턴 분석 및 wirelog 적용 가이드

**분석 대상**: GNOME libsoup (HTTP client/server library)
**분석 일자**: 2026-02-22
**목적**: libsoup의 검증된 설계 패턴을 wirelog 프로젝트에 적용하기 위한 가이드

---

## 1. libsoup 프로젝트 개요

### 1.1 프로젝트 특성
- **유형**: C 기반 라이브러리 (98.3% C)
- **규모**: 중규모 GNOME 프로젝트
- **목표**: GLib/GObject 기반 HTTP 클라이언트/서버 라이브러리
- **버전**: 3.x (현재)
- **라이센스**: LGPL-2.0-or-later
- **빌드 시스템**: Meson
- **문서**: 공식 API 문서 + 예제

**출처**: [GNOME/libsoup GitHub Repository](https://github.com/GNOME/libsoup), [Official API Documentation](https://gnome.pages.gitlab.gnome.org/libsoup/libsoup-3.0/index.html)

---

## 2. libsoup 디렉토리 구조 (상세)

### 2.1 최상위 구조

```
libsoup/
├── .gitlab-ci/              # CI/CD 설정
├── docs/reference/          # API 문서 (생성됨)
├── examples/                # 예제 애플리케이션
│   ├── get.c               # HTTP GET 클라이언트
│   ├── simple-httpd.c      # HTTP 서버
│   ├── simple-proxy.c      # 프록시 서버
│   ├── unix-socket-client.c # Unix 소켓 클라이언트
│   ├── unix-socket-server.c # Unix 소켓 서버
│   └── meson.build
├── fuzzing/                 # 퍼징 테스트
├── libsoup/                 # 핵심 라이브러리 소스
├── po/                      # 다국어 지원
├── subprojects/             # 외부 의존성
├── tests/                   # 테스트 스위트
├── meson.build              # 루트 빌드 설정
├── meson_options.txt        # Meson 옵션
├── README                   # 프로젝트 개요
├── COPYING                  # LGPL 라이센스
└── [config files: .clang-format, .clang-tidy, .editorconfig, AUTHORS, NEWS, etc.]
```

### 2.2 핵심 소스 디렉토리 구조 (libsoup/)

```
libsoup/
├── include/                 # Public API headers
│   ├── meson.build
│   └── soup-installed.h    # 설치된 헤더 설정
│
├── [프로토콜 구현]
├── http1/                   # HTTP/1.x 구현
│   ├── soup-body-input-stream.c/h
│   ├── soup-body-output-stream.c/h
│   ├── soup-client-message-io-http1.c/h
│   ├── soup-message-io-data.c/h
│   └── soup-message-io-source.c/h
│
├── http2/                   # HTTP/2 구현
│
├── [기능 모듈]
├── auth/                    # 인증 (Basic, Digest, NTLM, Negotiate)
├── cache/                   # 캐싱
├── content-decoder/         # Content 디코딩 (Gzip, Brotli)
├── content-sniffer/         # Content-type 감지
├── cookies/                 # 쿠키 관리 (text file, SQLite)
├── hsts/                    # HTTP Strict Transport Security
├── server/                  # 서버 구현
│   ├── http1/              # 서버 HTTP/1 구현
│   ├── http2/              # 서버 HTTP/2 구현
│   ├── soup-server.c/h
│   ├── soup-listener.c/h
│   ├── soup-server-connection.c/h
│   ├── soup-server-message.c/h
│   ├── soup-message-body.c/h
│   └── [auth domains]
│
├── websocket/               # WebSocket 지원
│
├── [핵심 유틸리티]
├── soup.h                   # Main public API entry point
├── soup-session.c/h         # 세션 관리
├── soup-message.c/h         # 메시지 처리
├── soup-headers.c/h         # 헤더 유틸리티
├── soup-uri-utils.c/h       # URI 파싱
├── soup-form.c/h            # Form data
├── soup-multipart.c/h       # Multipart 지원
├── soup-date.c/h            # 날짜 유틸리티
├── soup-logger.c/h          # 로깅
│
└── meson.build              # libsoup 빌드 설정
```

### 2.3 테스트 디렉토리 구조

```
tests/
├── resources/               # 공유 테스트 자원
├── autobahn/               # WebSocket 테스트
├── brotli-data/            # 압축 데이터
├── pkcs11/                 # PKCS#11 테스트
│
├── [인증 & 보안 테스트]
├── auth-test.c
├── server-auth-test.c
├── ntlm-test.c
├── samesite-test.c
├── ssl-test.c
│
├── [HTTP 프로토콜 테스트]
├── http2-test.c
├── http2-body-stream-test.c
├── connection-test.c
├── continue-test.c
├── coding-test.c
│
├── [데이터 처리 테스트]
├── chunk-io-test.c
├── multipart-test.c
├── request-body-test.c
├── streaming-test.c
├── brotli-decompressor-test.c
│
├── [클라이언트 기능 테스트]
├── cache-test.c
├── cookies-test.c
├── hsts-test.c
├── redirect-test.c
├── session-test.c
│
├── [서버 & 네트워크 테스트]
├── server-test.c
├── server-mem-limit-test.c
├── proxy-test.c
├── websocket-test.c
├── unix-socket-test.c
│
├── [유틸리티 테스트]
├── date-test.c
├── uri-parsing-test.c
├── header-parsing-test.c
├── logger-test.c
├── misc-test.c
│
├── [테스트 설정]
├── httpd/                  # Apache 설정 & 인증서
├── meson.build
└── [configuration files]
```

---

## 3. libsoup의 핵심 설계 패턴

### 3.1 모듈화 원칙

#### Pattern 1: 기능별 모듈 분리

**libsoup의 접근법**:
- 각 기능 영역(auth, cache, cookies, http1, http2 등)이 **독립적 디렉토리**로 구성
- 각 모듈은 공개/비공개 인터페이스를 명확히 분리
- 모듈 간 의존성은 최소화

**wirelog에 적용**:
```
wirelog/
├── include/               # Public API
│   ├── wirelog.h         # Main entry point
│   ├── wirelog-types.h   # Common types
│   └── wirelog-errors.h  # Error codes
│
├── logic/                # Core logic (parser, IR, optimizer)
│   ├── parser/
│   │   ├── parser.c/h
│   │   ├── lexer.c/h
│   │   └── ast.c/h
│   ├── ir/
│   │   ├── ir.c/h
│   │   ├── ir-builder.c/h
│   │   └── ir-printer.c/h    # Debug
│   ├── optimizer/
│   │   ├── optimizer.c/h      # Orchestrator
│   │   ├── passes/
│   │   │   ├── fusion.c/h
│   │   │   ├── jpp.c/h
│   │   │   ├── sip.c/h
│   │   │   └── sharing.c/h
│   │   └── stratify.c/h       # SCC detection
│   └── meson.build
│
├── io/                   # Input/Output
│   ├── csv.c/h          # CSV reader
│   ├── output.c/h       # Result output
│   └── meson.build
│
├── backend/             # Backend abstraction (Phase 3+)
│   ├── backend.h        # Abstract interface
│   ├── dd-backend.c/h   # DD executor (Phase 0-3)
│   └── (cpu-backend.c/h, fpga-backend.c/h 나중에)
│
└── tests/
    ├── unit/            # 단위 테스트
    │   ├── parser-test.c
    │   ├── ir-test.c
    │   ├── optimizer-test.c
    │   └── meson.build
    ├── integration/      # 통합 테스트
    │   ├── end-to-end-test.c
    │   ├── benchmark-test.c
    │   └── meson.build
    └── fixtures/         # 테스트 데이터
        ├── programs/     # Datalog 프로그램
        ├── data/        # CSV/Arrow 데이터
        └── expected/    # 예상 결과
```

#### Pattern 2: 계층별 의존성 방향

**libsoup**:
```
[Public API] → [Core Implementation] → [Protocol/Feature modules] → [I/O primitives]
```

**wirelog 권장**:
```
[Application API]
    ↓
[Logic Layer] (Parser → IR → Optimizer)
    ↓
[Backend Abstraction]
    ↓
[Backend Implementations] (DD, CPU, FPGA)
```

---

### 3.2 헤더 파일 구성

#### Pattern 3: Public API 통합 헤더

**libsoup의 패턴**:
```c
// soup.h - Main public API
#ifndef SOUP_H
#define SOUP_H

#include <libsoup/soup-version.h>

// 모든 public 기능 포함
#include <libsoup/soup-message.h>
#include <libsoup/soup-session.h>
#include <libsoup/soup-headers.h>
#include <libsoup/soup-uri-utils.h>
#include <libsoup/soup-form.h>
#include <libsoup/soup-multipart.h>
#include <libsoup/soup-date.h>
#include <libsoup/soup-logger.h>
#include <libsoup/soup-cookies.h>
#include <libsoup/soup-cache.h>
#include <libsoup/soup-auth.h>
#include <libsoup/soup-server.h>
#include <libsoup/soup-websocket.h>
// ... 더 많은 기능

#endif
```

**wirelog에 적용**:
```c
// wirelog.h - Main public API
#ifndef WIRELOG_H
#define WIRELOG_H

#include <wirelog/wirelog-version.h>
#include <wirelog/wirelog-types.h>
#include <wirelog/wirelog-errors.h>

// Parser API
#include <wirelog/wirelog-parser.h>

// IR API
#include <wirelog/wirelog-ir.h>

// Optimizer API
#include <wirelog/wirelog-optimizer.h>

// Executor API
#include <wirelog/wirelog-evaluator.h>

// I/O API
#include <wirelog/wirelog-io.h>

#endif
```

#### Pattern 4: Private vs Public Header 분리

**libsoup**:
- Public: `soup-xxx.h` (설치됨)
- Private: `soup-xxx-private.h` (라이브러리 내부만)
- Internal: 모듈별 `*-private.h`

**wirelog**:
```
include/
├── wirelog.h              # Public (사용자 볼 수 있음)
├── wirelog-parser.h       # Public API
├── wirelog-ir.h           # Public API
└── wirelog-optimizer.h    # Public API

logic/parser/
├── parser.c
├── parser.h               # Public (logic 모듈 내)
├── parser-private.h       # Private (parser 내부만)
├── lexer.c
└── lexer-private.h        # Private
```

---

### 3.3 명명 규칙 (Naming Convention)

#### Pattern 5: GLib/GObject 스타일 명명

**libsoup**:
- 타입: `SoupSession`, `SoupMessage`, `SoupHeaders` (CamelCase)
- 함수: `soup_session_new()`, `soup_message_get_headers()` (snake_case, `soup_` 프리픽스)
- Enums: `SOUP_STATUS_OK`, `SOUP_HTTP_1_1` (UPPER_SNAKE_CASE, `SOUP_` 프리픽스)
- Structs: `SoupCookie`, `SoupRange` (CamelCase)

**wirelog 권장**:
```c
// Types
typedef struct WirelogParser WirelogParser;
typedef struct WirelogIR WirelogIR;
typedef struct WirelogOptimizer WirelogOptimizer;
typedef struct WirelogEvaluator WirelogEvaluator;

// Functions
WirelogParser *wirelog_parser_new(void);
void wirelog_parser_free(WirelogParser *parser);
WirelogIR *wirelog_parser_parse(WirelogParser *parser, const char *program);
char *wirelog_ir_to_string(WirelogIR *ir);

// Enums
typedef enum {
    WIRELOG_ERROR_PARSE_ERROR,
    WIRELOG_ERROR_INVALID_PROGRAM,
    WIRELOG_ERROR_OPTIMIZATION_FAILED,
} WirelogError;

typedef enum {
    WIRELOG_OPTIMIZER_PASS_FUSION,
    WIRELOG_OPTIMIZER_PASS_JPP,
    WIRELOG_OPTIMIZER_PASS_SIP,
} WirelogOptimizerPass;

// Structures
typedef struct {
    char *relation_name;
    int arity;
    WirelogAttribute *attributes;
} WirelogRelation;
```

---

### 3.4 빌드 시스템 (Meson)

#### Pattern 6: 계층별 meson.build

**libsoup의 구조**:

1. **루트 meson.build** (전역 설정)
   ```meson
   project('libsoup', 'c', version: '3.7.0', license: 'LGPL-2.0-or-later')

   # 의존성 선언
   glib_dep = dependency('glib-2.0', version: '>= 2.70.0')
   gobject_dep = dependency('gobject-2.0')
   gio_dep = dependency('gio-2.0')
   # ... 더 많은 의존성

   # 컴파일러 설정
   # 플랫폼별 처리
   # 기능 감지
   ```

2. **libsoup/meson.build** (라이브러리 빌드)
   ```meson
   libsoup_sources = [
       'soup-session.c',
       'soup-message.c',
       'soup-headers.c',
       # ... 모든 소스 파일
   ]

   libsoup_public_headers = [
       'soup-session.h',
       'soup-message.h',
       # ... 공개 헤더
   ]

   libsoup_lib = library('soup-3.0',
       libsoup_sources,
       dependencies: [glib_dep, gobject_dep, gio_dep],
       install: true,
   )

   # 서브디렉토리 빌드
   subdir('auth')
   subdir('cache')
   subdir('http1')
   subdir('http2')
   # ...
   ```

3. **모듈별 meson.build** (auth/meson.build 예)
   ```meson
   auth_sources = [
       'soup-auth.c',
       'soup-auth-basic.c',
       'soup-auth-digest.c',
       # ...
   ]

   libsoup_sources += auth_sources  # 부모의 libsoup_sources에 추가
   ```

**wirelog에 적용**:

```meson
# Root meson.build
project('wirelog', 'c', version: '0.1.0', license: 'Apache-2.0')

cc = meson.get_compiler('c')

# 의존성
glib_dep = dependency('glib-2.0', version: '>= 2.70.0')
gobject_dep = dependency('gobject-2.0')

# Rust FFI (DD backend)
dd_rust_dep = dependency('differential-dataflow', method: 'pkg-config', required: false)

# 설정
conf = configuration_data()
conf.set_quoted('VERSION', meson.project_version())
conf.set('ENABLE_DD_BACKEND', dd_rust_dep.found())

configure_file(output: 'wirelog-config.h', configuration: conf)

# 서브디렉토리
subdir('include')
subdir('logic')
subdir('io')
subdir('backend')
subdir('tests')
```

```meson
# logic/meson.build
parser_sources = [
    'parser/parser.c',
    'parser/lexer.c',
    'parser/ast.c',
]

ir_sources = [
    'ir/ir.c',
    'ir/ir-builder.c',
    'ir/ir-printer.c',
]

optimizer_sources = [
    'optimizer/optimizer.c',
    'optimizer/passes/fusion.c',
    'optimizer/passes/jpp.c',
    'optimizer/passes/sip.c',
    'optimizer/passes/sharing.c',
    'optimizer/stratify.c',
]

logic_sources = parser_sources + ir_sources + optimizer_sources

# 라이브러리는 별도로 정의
libwirelog_core = library('wirelog-core',
    logic_sources,
    dependencies: [glib_dep, gobject_dep],
    install: true,
)
```

```meson
# tests/meson.build
test_deps = [glib_dep, gobject_dep]

executable('parser-test',
    'unit/parser-test.c',
    dependencies: [test_deps, libwirelog_core],
)

executable('ir-test',
    'unit/ir-test.c',
    dependencies: [test_deps, libwirelog_core],
)

# 통합 테스트
executable('end-to-end-test',
    'integration/end-to-end-test.c',
    dependencies: [test_deps, libwirelog_core],
)
```

---

### 3.5 테스트 조직화

#### Pattern 7: 계층별 테스트 분리

**libsoup**:
- **단위 테스트**: `auth-test.c`, `coding-test.c`, `uri-parsing-test.c`
- **기능 테스트**: `session-test.c`, `server-test.c`, `websocket-test.c`
- **통합 테스트**: Apache httpd 기반 복합 시나리오
- **성능/보안**: SSL test, memory limit test

**wirelog 권장**:

```
tests/
├── unit/
│   ├── parser-test.c          # Parser 단위 테스트
│   │   - Lexer
│   │   - AST construction
│   │   - Error handling
│   ├── ir-test.c              # IR 단위 테스트
│   │   - IR node creation
│   │   - IR traversal
│   │   - Memory management
│   ├── optimizer-test.c       # Optimizer 단위 테스트
│   │   - Each pass individually
│   │   - Cost model
│   │   - Join ordering
│   └── meson.build
│
├── integration/
│   ├── end-to-end-test.c      # Full pipeline
│   │   - Parse → IR → Optimize → Execute
│   │   - Multiple backends (DD, CPU if available)
│   ├── benchmark-test.c       # Performance testing
│   │   - Reach, CC, SSSP, TC
│   │   - Memory profiling
│   │   - Timing analysis
│   └── meson.build
│
├── fixtures/
│   ├── programs/              # Datalog 테스트 프로그램
│   │   ├── simple-reach.dl
│   │   ├── nested-reach.dl
│   │   ├── connected-components.dl
│   │   └── ... (표준 벤치마크)
│   ├── data/                  # CSV 입력 데이터
│   │   ├── graph-small.csv
│   │   ├── graph-large.csv
│   │   └── ...
│   └── expected/              # 예상 결과
│       ├── reach-expected.txt
│       └── ...
│
└── test-utils.c/h             # 공유 테스트 유틸
    - File I/O helpers
    - Comparison utilities
    - Memory tracking
```

---

### 3.6 라이브러리 vs 애플리케이션 구조

#### Pattern 8: 라이브러리 코어 + 예제 분리

**libsoup**:
- **libsoup-3.0** (라이브러리): 재사용 가능한 HTTP 기능
- **examples/**: 라이브러리를 사용하는 독립적 프로그램
  - `get.c`: HTTP 클라이언트
  - `simple-httpd.c`: HTTP 서버
  - 각각 독립적으로 컴파일 가능

**wirelog 권장**:

```
wirelog/
├── [라이브러리 코어]
├── libwirelog-core.a/so    # libsoup처럼 재사용 가능
│   └── Parser, IR, Optimizer, Backend abstraction
│
├── examples/               # 라이브러리 사용 예제
│   ├── simple-query.c     # 단순 Datalog 쿼리
│   ├── csv-processor.c    # CSV 처리 예제
│   ├── benchmark-runner.c # 벤치마크 실행기
│   └── meson.build
│
└── [CLI 도구] (선택사항)
    ├── wirelog-cli.c      # Command-line interface
    └── meson.build
```

---

## 4. wirelog에 적용할 구체적 패턴

### 4.1 파일 조직화 (권장)

**Phase 0 구조** (현재):

```
wirelog/
├── .gitignore
├── .clang-format              # libsoup 스타일 차용 가능
├── .clang-tidy
├── meson.build                # 루트
├── meson_options.txt
│
├── include/
│   ├── wirelog.h              # Main API entry point
│   ├── wirelog-version.h      # Version macros
│   ├── wirelog-types.h        # Common types (WirelogProgram, WirelogIR, etc.)
│   ├── wirelog-errors.h       # Error codes & handling
│   ├── wirelog-parser.h       # Parser API
│   ├── wirelog-ir.h           # IR API
│   ├── wirelog-optimizer.h    # Optimizer API
│   ├── wirelog-evaluator.h    # Evaluation API
│   ├── wirelog-io.h           # I/O API
│   └── meson.build            # Install headers
│
├── src/ (또는 libsoup 스타일대로 wirelog/ 사용)
│   ├── meson.build            # libwirelog-core target
│   │
│   ├── parser/
│   │   ├── parser.c/h         # Main parser
│   │   ├── lexer.c/h          # Tokenization
│   │   ├── ast.c/h            # AST representation
│   │   ├── parser-private.h   # Internal only
│   │   └── meson.build        # 선택: 개별 서브빌드
│   │
│   ├── ir/
│   │   ├── ir.c/h             # IR core
│   │   ├── ir-builder.c/h     # Builder pattern
│   │   ├── ir-printer.c/h     # Debug printing
│   │   ├── ir-private.h
│   │   └── meson.build        # 선택
│   │
│   ├── optimizer/
│   │   ├── optimizer.c/h      # Orchestrator
│   │   ├── optimizer-private.h
│   │   ├── stratify.c/h       # SCC detection
│   │   ├── passes/
│   │   │   ├── fusion.c/h     # Logic fusion
│   │   │   ├── jpp.c/h        # Join-Project Plan
│   │   │   ├── sip.c/h        # Semijoin IP
│   │   │   ├── sharing.c/h    # Subplan sharing
│   │   │   └── meson.build    # 선택
│   │   └── meson.build        # 선택
│   │
│   └── io/
│       ├── csv.c/h            # CSV reader
│       ├── output.c/h         # Result formatter
│       └── meson.build        # 선택
│
├── backend/ (Phase 3에 추가)
│   ├── backend.h              # Abstract interface
│   ├── dd-backend.c/h         # DD FFI (Phase 0)
│   └── meson.build
│
├── tests/
│   ├── meson.build
│   ├── unit/
│   │   ├── parser-test.c
│   │   ├── ir-test.c
│   │   ├── optimizer-test.c
│   │   ├── stratify-test.c
│   │   ├── passes-test.c      # 각 최적화 pass
│   │   ├── test-utils.c/h
│   │   └── meson.build
│   ├── integration/
│   │   ├── end-to-end-test.c  # Full pipeline
│   │   ├── benchmark-test.c   # Performance
│   │   └── meson.build
│   └── fixtures/
│       ├── programs/          # .dl files
│       ├── data/              # .csv files
│       └── expected/          # Expected results
│
├── examples/
│   ├── simple-query.c
│   ├── csv-processor.c
│   └── meson.build
│
├── docs/
│   ├── ARCHITECTURE.md        # 이미 있음
│   ├── API.md                 # API 문서
│   ├── DEVELOPMENT.md         # 개발 가이드
│   └── LIBSOUP_PATTERNS_FOR_WIRELOG.md (이 문서)
│
└── .omc/, .claude/ (프로젝트 관리)
```

### 4.2 meson.build 구조 (구체적)

**root/meson.build**:
```meson
project('wirelog', 'c',
    version: '0.1.0',
    license: 'Apache-2.0',
    default_options: [
        'c_std=c99',
        'warning_level=2',
        'buildtype=release',
    ],
    meson_version: '>=0.62.0',
)

# 컴파일러 설정
cc = meson.get_compiler('c')

# 의존성
glib_dep = dependency('glib-2.0', version: '>= 2.70.0')
gobject_dep = dependency('gobject-2.0')

# 부모의 소스 리스트 (libsoup 패턴)
wirelog_sources = []
wirelog_public_headers = []
wirelog_deps = [glib_dep, gobject_dep]

# 서브디렉토리 (각 subdir는 wirelog_sources에 추가)
subdir('include')
subdir('src')
subdir('backend')
subdir('tests')
subdir('examples')

# 메인 라이브러리
libwirelog = library('wirelog-0',
    wirelog_sources,
    dependencies: wirelog_deps,
    install: true,
    include_directories: include_directories('include', 'src'),
)

# 설치 설정
install_headers(wirelog_public_headers, subdir: 'wirelog')

# pkg-config 설정
pkgconfig = import('pkgconfig')
pkgconfig.generate(libwirelog,
    description: 'Wirelog - Embedded-to-Enterprise Datalog Engine',
    version: meson.project_version(),
)
```

**src/meson.build**:
```meson
# 각 모듈의 소스 정의
parser_sources = files(
    'parser/parser.c',
    'parser/lexer.c',
    'parser/ast.c',
)

ir_sources = files(
    'ir/ir.c',
    'ir/ir-builder.c',
    'ir/ir-printer.c',
)

optimizer_sources = files(
    'optimizer/optimizer.c',
    'optimizer/stratify.c',
    'optimizer/passes/fusion.c',
    'optimizer/passes/jpp.c',
    'optimizer/passes/sip.c',
    'optimizer/passes/sharing.c',
)

io_sources = files(
    'io/csv.c',
    'io/output.c',
)

# 부모 리스트에 추가
wirelog_sources += parser_sources
wirelog_sources += ir_sources
wirelog_sources += optimizer_sources
wirelog_sources += io_sources
```

**include/meson.build**:
```meson
wirelog_public_headers += files(
    'wirelog.h',
    'wirelog-version.h',
    'wirelog-types.h',
    'wirelog-errors.h',
    'wirelog-parser.h',
    'wirelog-ir.h',
    'wirelog-optimizer.h',
    'wirelog-evaluator.h',
    'wirelog-io.h',
)
```

---

### 4.3 API 설계 (libsoup 스타일)

**wirelog.h** (Main entry point):
```c
#ifndef WIRELOG_H
#define WIRELOG_H

#include <glib.h>
#include <wirelog/wirelog-version.h>
#include <wirelog/wirelog-types.h>
#include <wirelog/wirelog-errors.h>
#include <wirelog/wirelog-parser.h>
#include <wirelog/wirelog-ir.h>
#include <wirelog/wirelog-optimizer.h>
#include <wirelog/wirelog-evaluator.h>
#include <wirelog/wirelog-io.h>

G_BEGIN_DECLS

/* Version checking */
#define WIRELOG_CHECK_VERSION(major, minor, patch) \
    (WIRELOG_MAJOR_VERSION > (major) || \
     (WIRELOG_MAJOR_VERSION == (major) && \
      WIRELOG_MINOR_VERSION > (minor)) || \
     (WIRELOG_MAJOR_VERSION == (major) && \
      WIRELOG_MINOR_VERSION == (minor) && \
      WIRELOG_PATCH_VERSION >= (patch)))

G_END_DECLS

#endif /* WIRELOG_H */
```

**wirelog-types.h**:
```c
#ifndef WIRELOG_TYPES_H
#define WIRELOG_TYPES_H

#include <glib.h>

G_BEGIN_DECLS

/* Opaque types */
typedef struct _WirelogParser WirelogParser;
typedef struct _WirelogProgram WirelogProgram;
typedef struct _WirelogIR WirelogIR;
typedef struct _WirelogOptimizer WirelogOptimizer;
typedef struct _WirelogOptimizedIR WirelogOptimizedIR;
typedef struct _WirelogEvaluator WirelogEvaluator;
typedef struct _WirelogResult WirelogResult;

/* Simple structs */
typedef struct {
    char *name;
    int arity;
    char **arg_names;
} WirelogRelation;

typedef struct {
    char *relation;
    int *tuple;
} WirelogFact;

/* Error handling */
typedef enum {
    WIRELOG_ERROR_NONE = 0,
    WIRELOG_ERROR_PARSE_ERROR,
    WIRELOG_ERROR_INVALID_PROGRAM,
    WIRELOG_ERROR_OPTIMIZATION_FAILED,
    WIRELOG_ERROR_EXECUTION_FAILED,
    WIRELOG_ERROR_IO_ERROR,
} WirelogError;

G_END_DECLS

#endif /* WIRELOG_TYPES_H */
```

---

### 4.4 테스트 작성 패턴

**tests/unit/parser-test.c** (libsoup 스타일):
```c
#include <glib.h>
#include <wirelog/wirelog-parser.h>

static void
test_parse_simple_rule(void)
{
    WirelogParser *parser;
    WirelogProgram *program;
    GError *error = NULL;

    parser = wirelog_parser_new();
    g_assert_nonnull(parser);

    program = wirelog_parser_parse(parser,
        "reach(X, Y) :- edge(X, Y).\n"
        "reach(X, Z) :- edge(X, Y), reach(Y, Z).",
        &error);

    g_assert_null(error);
    g_assert_nonnull(program);

    wirelog_program_free(program);
    wirelog_parser_free(parser);
}

static void
test_parse_error_handling(void)
{
    WirelogParser *parser;
    WirelogProgram *program;
    GError *error = NULL;

    parser = wirelog_parser_new();
    program = wirelog_parser_parse(parser, "invalid :::", &error);

    g_assert_null(program);
    g_assert_nonnull(error);
    g_assert_true(g_error_matches(error, WIRELOG_PARSE_ERROR, 0));

    g_error_free(error);
    wirelog_parser_free(parser);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/wirelog/parser/simple-rule",
                    test_parse_simple_rule);
    g_test_add_func("/wirelog/parser/error-handling",
                    test_parse_error_handling);

    return g_test_run();
}
```

---

## 5. wirelog 적용 체크리스트

### 5.1 파일 구조 설정 (Phase 0)

- [ ] `include/` 생성 및 public headers 작성
  - [ ] `wirelog.h` (main entry point)
  - [ ] `wirelog-version.h`, `wirelog-types.h`, `wirelog-errors.h`
  - [ ] `wirelog-parser.h`, `wirelog-ir.h`, `wirelog-optimizer.h`

- [ ] `src/` 생성 및 모듈 분리
  - [ ] `src/parser/` (parser.c/h, lexer.c/h, ast.c/h)
  - [ ] `src/ir/` (ir.c/h, ir-builder.c/h, ir-printer.c/h)
  - [ ] `src/optimizer/` (optimizer.c/h, stratify.c/h)
  - [ ] `src/optimizer/passes/` (fusion.c/h, jpp.c/h, sip.c/h, sharing.c/h)
  - [ ] `src/io/` (csv.c/h, output.c/h)

- [ ] 명명 규칙 적용
  - [ ] 타입: `WirelogXxx` (CamelCase)
  - [ ] 함수: `wirelog_xxx_yyy()` (snake_case, `wirelog_` 프리픽스)
  - [ ] Enums: `WIRELOG_XXX_YYY` (UPPER_SNAKE_CASE)

### 5.2 빌드 시스템 (Phase 0)

- [ ] Meson 설정 정밀화
  - [ ] Root `meson.build` 정리
  - [ ] `meson_options.txt` 정의 (C standard, optimization level, etc.)
  - [ ] 모듈별 `meson.build` 추가 (선택적이지만 권장)

- [ ] 의존성 관리
  - [ ] GLib/GObject 의존성 명시
  - [ ] DD Rust FFI 의존성 (Phase 0에는 선택적)

### 5.3 테스트 (Phase 0)

- [ ] 단위 테스트 구조
  - [ ] `tests/unit/parser-test.c`
  - [ ] `tests/unit/ir-test.c`
  - [ ] `tests/unit/optimizer-test.c`

- [ ] 테스트 자산
  - [ ] `tests/fixtures/programs/` (Datalog 프로그램)
  - [ ] `tests/fixtures/data/` (CSV 데이터)
  - [ ] `tests/fixtures/expected/` (예상 결과)

- [ ] GLib gtest 패턴 사용
  - [ ] `g_test_init()` / `g_test_add_func()`
  - [ ] `g_assert_*()` 매크로
  - [ ] `GError` 오류 처리

### 5.4 문서화

- [ ] Public API 문서 (GLib gtk-doc 스타일)
  - [ ] 각 함수의 `/**` 코멘트
  - [ ] 파라미터 설명
  - [ ] 반환값 설명
  - [ ] 에러 조건

- [ ] DEVELOPMENT.md 작성
  - [ ] 빌드 방법
  - [ ] 테스트 실행 방법
  - [ ] 각 모듈 책임 설명

---

## 6. wirelog vs libsoup 비교

| 측면 | libsoup | wirelog (권장) |
|------|---------|----------------|
| **언어** | C (98.3%) | C11 |
| **기능 조직** | 프로토콜별 (HTTP/1, HTTP/2) | 논리 계층별 (Parser, IR, Optimizer) |
| **라이브러리 구조** | 모듈식 (auth/, cache/ 등) | 계층식 (logic/, io/, backend/) |
| **테스트** | 60개 + fuzzing | Unit + integration (단계별) |
| **빌드 시스템** | Meson | Meson |
| **명명** | `soup_*()` | `wirelog_*()` |
| **Header** | Public wrapper (`soup.h`) | Public wrapper (`wirelog.h`) |
| **백엔드 추상화** | SessionFeature | ComputeBackend (Phase 3+) |
| **플랫폼** | Linux, Windows, macOS | Embedded, Enterprise, FPGA (미래) |

---

## 7. 참고 자료

### libsoup 공식 자료
- [GNOME/libsoup GitHub](https://github.com/GNOME/libsoup) - 읽기 전용 미러
- [Official libsoup API Documentation](https://gnome.pages.gitlab.gnome.org/libsoup/libsoup-3.0/index.html)
- [Meson Build System](https://mesonbuild.com/)
- [GLib Reference Manual](https://gnome.pages.gitlab.gnome.org/libsoup/glib/)

### wirelog 관련 문서
- `/Users/joykim/git/claude/discuss/wirelog/docs/ARCHITECTURE.md` - 현재 설계
- `/Users/joykim/git/claude/discuss/wirelog/discussion/FlowLog_C_Implementation_Analysis.md`
- Differential Dataflow: https://github.com/TimelyDataflow/differential-dataflow

---

## 8. 다음 단계 (Action Items)

### Phase 0 (Week 1-2)

1. [ ] 디렉토리 구조 생성
   ```bash
   mkdir -p src/parser src/ir src/optimizer/{passes} src/io backend tests/{unit,integration,fixtures/{programs,data,expected}} examples
   ```

2. [ ] Public header skeleton 작성
   - [ ] `include/wirelog.h`
   - [ ] `include/wirelog-types.h`
   - [ ] 나머지 public headers

3. [ ] Meson 빌드 파일 정리
   - [ ] Root `meson.build` 업데이트
   - [ ] 모듈별 `meson.build` 추가

4. [ ] Parser 구현 시작 (기존 코드 기반)
   - [ ] `src/parser/parser.h` 설계
   - [ ] `src/parser/ast.h` 정의

5. [ ] 첫 번째 단위 테스트 작성
   - [ ] `tests/unit/parser-test.c`

### Phase 1 (Week 3+)

- IR 계층 구현
- Optimizer 기본 구조
- 통합 테스트 작성
- 벤치마크 프로토타입

---

**작성자**: Document Specialist (oh-my-claudecode)
**최종 수정**: 2026-02-22
