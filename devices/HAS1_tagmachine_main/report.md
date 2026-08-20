# 태그머신 변경 보고서

작성일: 2026-04-23

---

## 변경 요약

이번 업데이트에서는 태그머신의 상태 불일치 문제와 뉴비모드 동작 문제를 수정했다.

크게 바뀐 내용은 다음과 같다.

1. `CancelTagProgress()`를 추가해 태그 진행 취소 상황을 확실히 정리했다.
2. 보라색 게이지가 내려가다가 멈추는 문제를 수정했다.
3. 게이지 연출 중 서버에서 받은 `device_state`가 나중에 장치에 반영되지 않는 문제를 수정했다.
4. 뉴비모드에서 `activate`를 받으면 즉시 서버와 로컬 상태를 `lock`으로 맞추도록 수정했다.
5. 뉴비모드 역할별 동작을 추가하고 정리했다.
6. `CancelTagProgress()` undefined reference 컴파일 에러를 수정했다.

---

## 변경된 파일

### `timer.ino`

- `CancelTagProgress()` 함수를 추가했다.
- `TimerRun()` 마지막에 `ApplyPendingDeviceState()`를 호출하도록 했다.
- 태그 진행 중 서버 상태가 예상과 다르면 `loginDone = false`만 하지 않고 `CancelTagProgress()`로 정리하도록 바꿨다.
- 다음 타이머 함수들의 상태 불일치 처리를 수정했다.
  - `PlayerLockTimerFunc()`
  - `PlayerUnlockTimerFunc()`
  - `TaggerUnlockTimerFunc()`
  - `GhostUnlockTimerFunc()`
  - `NewbieTaggerUnlockTimerFunc()`
  - `GhostLockTimerFunc()`
- `SubSerialTimerFunc()`에서 타이머 종료 후 `SubSerialTimerStart`를 `false`로 맞추도록 수정했다.

### `rfid.ino`

- `NewbieTaggerFail()`, `LockFail()`, `UnlockFail()`에서 서버 상태가 예상과 다를 때 `CancelTagProgress()`를 호출하도록 수정했다.
- 실패 상황에서도 타이머와 RFID 포인터가 남아 장치가 애매한 상태로 멈추지 않도록 했다.
- 뉴비모드에서 easy+lock 상태일 때 역할별 전용 흐름으로 연결되도록 했다.

### `Wifi.ino`

- 서버에서 받은 `device_state`를 즉시 적용하는 흐름과 나중에 적용하는 흐름을 분리했다.
- `QueuePendingDeviceState()`를 추가했다.
- `ApplyPendingDeviceState()`를 추가했다.
- `ApplyDeviceState()`를 추가했다.
- 뉴비모드에서 `ActivateFunc()`가 실행되면 서버에 `device_state=lock`을 다시 보내고, 로컬 상태도 즉시 `lock`으로 맞추도록 했다.

### `HAS1_tagmachine_main.h`

- 새 함수 선언을 추가했다.
  - `ApplyDeviceState()`
  - `QueuePendingDeviceState()`
  - `ApplyPendingDeviceState()`
  - `CancelTagProgress()`
- 서버 상태 보류용 변수를 추가했다.
  - `pendingDeviceState`
  - `pendingDeviceStateApply`

### `Game_system.ino`

- 뉴비모드 플레이어/유령 문 열림 함수가 추가 및 정리되었다.
- 문 열림 연출 후 서버 상태를 다시 `lock`으로 돌리고 네오픽셀을 초록색으로 맞추도록 했다.
- `NewbiePlayerOpen()`의 효과음을 `VD7`에서 `VD1`로 변경했다.

---

## 문제 1: `CancelTagProgress()` 컴파일 에러

### 문제

코드 여러 곳에서 `CancelTagProgress()`를 호출하고 있었지만, 실제 함수 정의 이름이 깨져 있어서 링커가 함수를 찾지 못했다.

발생한 에러는 다음과 같았다.

```text
undefined reference to `CancelTagProgress()'
```

### 원인

헤더와 호출부는 `CancelTagProgress()`를 기대했지만, 실제 구현 이름에 깨진 문자가 섞여 있었다.

그래서 컴파일은 진행되지만 링크 단계에서 함수 정의를 찾지 못했다.

### 수정

`timer.ino`에 정확한 이름으로 함수를 정의하고, `HAS1_tagmachine_main.h`에 선언을 추가했다.

```cpp
void CancelTagProgress();
```

```cpp
void CancelTagProgress(){
    GameTimer.deleteTimer(gameTimerId);
    SubSerialTimer.deleteTimer(subSerialTimerId);
    SubSerialTimerStart = false;
    gameTimerCnt = 0;
    loginDone = false;

    ptrRfidMain = CommnunicationMainBeetle;
    ptrRfidSub = CommnunicationBeetle;
    ptrRfidMode = Login;
    ptrRfidFail = WaitFunc;

    SubSerialFlush();
    MainSerialFlush();
    Serial.println("Cancel Tag Progress");
    ApplyPendingDeviceState();
}
```

### 결과

`CancelTagProgress()`의 선언, 정의, 호출 이름이 모두 같아져 링크 에러가 해결되었다.

---

## 문제 2: 보라색 게이지가 내려가다가 멈춤

### 문제

술래가 잠금해제를 시도할 때 보라색 게이지가 내려가다가 중간에 멈추는 현상이 있었다.

관련 함수는 다음이다.

- `TaggerUnlockTimerFunc()`
- `NewbieTaggerUnlockTimerFunc()`

### 원인

기존 코드는 게이지 진행 중 서버 상태가 예상과 달라졌을 때 `loginDone = false`만 실행했다.

```cpp
if(strCurState != "lock"){
    Serial.println("debuff on");
    loginDone = false;
}
```

이 방식은 로그인 플래그만 내릴 뿐, 실제 진행 상태를 정리하지 못했다.

남아 있을 수 있던 값은 다음과 같다.

- `GameTimer`
- `SubSerialTimer`
- `SubSerialTimerStart`
- `gameTimerCnt`
- `ptrRfidMain`
- `ptrRfidSub`
- `ptrRfidMode`
- `ptrRfidFail`
- 시리얼 버퍼

그래서 장치는 취소되어야 하는 상황인데도 내부 상태는 태그 진행 중처럼 남았다. 이 때문에 게이지가 멈춘 것처럼 보였다.

### 수정

상태가 맞지 않으면 `loginDone = false`만 하지 않고 `CancelTagProgress()`를 호출하도록 바꿨다.

```cpp
if(strCurState != "lock"){
    Serial.println("debuff on");
    CancelTagProgress();
}
```

### 결과

태그 진행이 취소될 때 타이머, 포인터, 카운터, 시리얼 버퍼가 함께 정리된다. 장치가 게이지 중간 상태에 남지 않고 정상 대기 상태로 돌아간다.

---

## 문제 3: 서버에서 `lock`을 받아도 연출 후 반영되지 않음

### 문제

문 열림/잠금 연출 중 서버에서 `device_state=lock`을 받아도, 연출이 끝난 뒤 네오픽셀이나 장치 상태에 반영되지 않는 문제가 있었다.

예를 들어 동그라미 게이지가 차오르는 중에 서버에서 `lock`이 들어오면 `DataChanged()`는 값을 받는다. 하지만 게이지 함수가 계속 네오픽셀을 덮어써서 최종 LED가 서버 상태와 다르게 남을 수 있었다.

### 원인

`DataChanged()`는 서버값을 한 번 처리한 뒤 마지막에 `cur = my`로 저장한다.

```cpp
cur = my;
```

이후 게이지 함수가 LED를 다시 덮어쓰면, 서버의 `lock` 값은 이미 처리된 것으로 기록되어 다시 반영될 기회가 사라진다.

### 수정

서버 상태를 인지하는 것과 실제 장치에 반영하는 것을 분리했다.

태그/게이지 진행 중에는 바로 LED를 바꾸지 않고 pending 상태로 저장한다.

```cpp
if(deviceState == "lock"){
  strCurState = deviceState;
  if(loginDone) QueuePendingDeviceState(deviceState);
  else ApplyDeviceState(deviceState);
}
else if(deviceState == "activate"){
  strCurState = deviceState;
  if(loginDone) QueuePendingDeviceState(deviceState);
  else ApplyDeviceState(deviceState);
}
```

보류 함수는 다음과 같다.

```cpp
void QueuePendingDeviceState(String deviceState) {
    pendingDeviceState = deviceState;
    pendingDeviceStateApply = true;
    Serial.println("Queue Device State: " + deviceState);
}
```

진행이 끝난 뒤 적용한다.

```cpp
void ApplyPendingDeviceState() {
    if (!pendingDeviceStateApply || loginDone) return;

    String deviceState = pendingDeviceState;
    pendingDeviceState = "";
    pendingDeviceStateApply = false;
    ApplyDeviceState(deviceState);
}
```

### 결과

- 서버 상태는 `strCurState`에 즉시 반영된다.
- 네오픽셀과 장치 상태는 게이지 진행이 끝난 뒤 반영된다.
- `cur = my` 때문에 서버 변경이 다시 감지되지 않는 문제를 pending 변수로 해결했다.

---

## 문제 4: 뉴비모드 추가 및 동작 정리

### 추가된 동작

뉴비모드는 서버의 `mode` 값이 `"easy"`이고 `game_state`가 `"activate"`일 때 활성화된다.

```cpp
void NewbieModeSetting() {
    if ((String)(const char*)my["mode"] == "easy" &&
        (String)(const char*)my["game_state"] == "activate") {
        ptrRfidMode = NewbieLogin;
    }
}
```

이 조건이 만족되면 일반 `Login()` 흐름 대신 `NewbieLogin()`을 기본 RFID 처리 함수로 사용한다.

### 역할별 동작

뉴비모드에서 역할별 흐름은 다음과 같다.

```cpp
void NewbieLogin(char role) {
    if (role == 'T') {
        Login(role);
    } else if (role == 'P') {
        NewbiePlayerOpen();
    } else if (role == 'G') {
        NewbieGhostOpen();
    }
}
```

플레이어:

- `NewbiePlayerOpen()` 실행
- 서버에 `device_state=open` 전송
- 초록색 문 열림 연출
- 문 열림 연출 후 서버에 `device_state=lock` 전송
- 네오픽셀을 초록색으로 변경

유령:

- `NewbieGhostOpen()` 실행
- 서버에 `device_state=open` 전송
- 파란색 문 열림 연출
- 문 열림 연출 후 서버에 `device_state=lock` 전송
- 네오픽셀을 초록색으로 변경

술래:

- 바로 문을 열지 않고 `Login(role)` 흐름을 탄다.
- `LoginTimerSelector()`에서 `NewbieTaggerUnlockTimerFunc()`로 연결된다.
- 보라색 게이지가 진행된다.
- 게이지 완료 후 문을 열고 서버에 `device_state=open`을 보낸다.
- 연출 후 서버에 `device_state=lock`을 다시 보낸다.
- `ptrRfidMode = NewbieLogin`으로 뉴비모드 흐름을 유지한다.

### 결과

뉴비모드에서는 누가 문을 열어도 최종 상태가 다시 `lock`으로 복귀한다.

---

## 문제 5: 뉴비모드에서 `activate`를 받으면 즉시 `lock`이어야 함

### 문제

뉴비모드에서는 서버에서 `device_state=activate`가 내려와도 장치가 열린 상태로 남으면 안 된다.

요구사항은 다음과 같았다.

1. 서버에서 `device_state=activate`를 받는다.
2. 현재 모드가 뉴비모드라면 서버에 다시 `device_state=lock`을 보낸다.
3. 로컬 장치 상태도 즉시 `lock`으로 맞춘다.
4. 네오픽셀도 초록색 잠금 상태로 표시한다.

### 수정

`ActivateFunc()` 안에 easy 모드 예외 처리를 추가했다.

```cpp
if ((String)(const char*)my["mode"] == "easy") {
    has2wifi.Send((String)(const char*)my["device_name"], "device_state", "lock");
    my["device_state"] = "lock";
    strCurState = "lock";
    AllNeoOn(GREEN);
}
```

### 결과

뉴비모드에서는 `activate`를 받아도 즉시 서버와 로컬 상태가 `lock`으로 맞춰진다.

중요한 점은 `my["device_state"] = "lock";`도 함께 넣었다는 것이다. 서버에서 받은 원본 값은 `activate`이므로, 로컬 JSON 값을 바꾸지 않으면 이후 코드에서 다시 `activate`처럼 판단될 수 있기 때문이다.

---

## 커밋 정보

- `3d2e42e` - `Fix tag progress cancellation flow`
- `acef123` - `Add tag progress fix report`
- `fb09151` - `Defer device state application during tag progress`
- `fc25c29` - `Force newbie mode activate state to lock`

브랜치: `beetle_2`
