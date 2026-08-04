# 펌웨어와 HTTP 동작

## P4 데이터 수집

`firmware/p4_data`는 C6의 무선을 ESP Wi-Fi Remote로 사용해 SoftAP를 엽니다. PC나 휴대폰을 `esp32p4-data`에 연결한 뒤 `http://192.168.4.1/`로 접속합니다.

촬영 시 카메라 스트림의 완료 버퍼를 먼저 순환하고 요청 이후 생성된 프레임을 기다립니다. 따라서 오랫동안 요청이 없었더라도 `/pic`, `/record`, `/capture`가 과거의 정지 프레임을 사용하지 않습니다. 카메라/JPEG 자원은 mutex로 직렬화되어 동시에 여러 HTTP 요청이나 GPIO 촬영이 들어와도 같은 버퍼를 덮어쓰지 않습니다.

SD 카드가 없거나 마운트에 실패한 경우에도 서버는 시작됩니다. 이때 `/pic`과 `/record`는 사용할 수 있고 저장 관련 경로만 오류를 반환합니다.

## P4 추론

`firmware/p4_inference`는 P4의 Ethernet 인터페이스에 HTTP 서버를 엽니다. 부팅 로그의 Ethernet IP를 사용합니다.

| URL | 설명 |
| --- | --- |
| `/pic` | 요청 이후 생성된 원본 화면의 JPEG |
| `/record` | 요청 이후 생성된 RGB565 바이트 |
| `/classify.jpg` | 요청 이후 생성된 화면을 추론하고 JPEG 반환 |

분류 응답 헤더는 다음과 같습니다.

```text
X-Class-Index: 1
X-Class-Label: human
X-Class-Score: 0.9821
X-Inference-Time-Ms: 1342.50
X-Inference-Total-Ms: 1362.10
```

P4 자체 HTTP 결과는 C6 전송과 독립적입니다. C6 custom RPC가 응답하지 않으면 전송 작업을 재부팅 전까지 비활성화하고 P4 HTTP와 추론은 계속 동작합니다.

## C6 Hosted

`firmware/c6_hosted`는 Hosted 보조 칩 역할과 함께 P4가 보낸 JPEG를 메모리에 보관합니다. C6가 연결된 공유기에서 받은 IP와 포트 `8081`을 사용합니다. C6에 설정된 SSID/비밀번호가 placeholder이면 IP를 받지 못하므로 `menuconfig`에서 먼저 수정해야 합니다.

브라우저의 `/favicon.ico` 요청에 대한 `404`나, 페이지를 새로 고칠 때 끊어진 이전 소켓에서 발생하는 `httpd_sock_err: error in send : 104`는 보통 촬영·추론 실패를 뜻하지 않습니다. 반복적으로 실제 JPEG 요청까지 실패할 때만 네트워크와 메모리를 추가 점검합니다.
