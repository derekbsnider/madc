# js:: Namespace

JavaScript/web-oriented functions: base64 encoding/decoding, URL encoding/decoding, parseInt with radix support, and JSON serialization.

## Base64

| Function | Description | Example |
|----------|-------------|---------|
| `btoa(result, input)` | Base64 encode | `js::btoa(encoded, s)` |
| `atob(result, input)` | Base64 decode | `js::atob(decoded, encoded)` |

## URL Encoding

| Function | Description | Example |
|----------|-------------|---------|
| `encodeURIComponent(result, input)` | URL-encode a string | `js::encodeURIComponent(url, s)` |
| `decodeURIComponent(result, input)` | URL-decode a string | `js::decodeURIComponent(s, url)` |

## Parsing

| Function | Description | Example |
|----------|-------------|---------|
| `parseInt(str, radix)` | Parse integer with base | `n = js::parseInt(s, 16)` — hex to int |

Supported radixes: 2-36. Common uses:
- `js::parseInt(s, 16)` — hexadecimal
- `js::parseInt(s, 8)` — octal
- `js::parseInt(s, 2)` — binary
- `js::parseInt(s, 10)` — decimal (explicit)

## JSON

| Function | Description | Example |
|----------|-------------|---------|
| `stringify(result, arr)` | Serialize array to JSON | `js::stringify(json, a)` |

Serializes a MadArray to a JSON array string. String values are quoted and escaped, integers and doubles are bare.

## Example

```c
int main()
{
    // base64 round-trip
    string original = "Hello, World!";
    string encoded;
    string decoded;
    js::btoa(encoded, original);
    js::atob(decoded, encoded);
    cout << encoded << endl;    // SGVsbG8sIFdvcmxkIQ==
    cout << decoded << endl;    // Hello, World!

    // URL encoding
    string query = "name=John Doe&age=30";
    string url;
    js::encodeURIComponent(url, query);
    cout << url << endl;        // name%3DJohn%20Doe%26age%3D30

    // hex parsing
    string hex = "ff";
    int n;
    n = js::parseInt(hex, 16);
    cout << n << endl;          // 255

    // JSON
    array data;
    php::array_push(data, "hello");
    php::array_push(data, 42);
    string json;
    js::stringify(json, data);
    cout << json << endl;       // ["hello",42]

    return 0;
}
```
