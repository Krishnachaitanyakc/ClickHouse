---
description: 'Documentation for AI Functions'
sidebar_label: 'AI'
slug: /sql-reference/functions/ai-functions
title: 'AI Functions'
doc_type: 'reference'
---

# AI functions

AI Functions are built-in functions in ClickHouse that you can use to call AI or generate embeddings to work with your data, extract information, classify data, etc...

:::note
AI functions can return unpredictable inputs. The result will highly depend on the quality of the prompt and the model used.
:::

All functions are sharing a common infrastructure that provides:

- **Deduplication**: Identical inputs within the same query are sent to the provider only once.
- **Result caching**: Responses are cached with configurable TTL ([`ai_cache_ttl_sec`](/operations/settings/settings#ai_cache_ttl_sec)) to avoid redundant API calls.
- **Concurrency**: Multiple API requests are dispatched in parallel (configurable via [`ai_max_concurrent_requests`](/operations/settings/settings#ai_max_concurrent_requests)).
- **Rate limiting**: Requests per second can be limited using [`ai_max_rps`](/operations/settings/settings#ai_max_rps).
- **Quota enforcement**: Per-query limits on rows ([`ai_max_rows_per_query`](/operations/settings/settings#ai_max_rows_per_query)), tokens ([`ai_max_input_tokens_per_query`](/operations/settings/settings#ai_max_input_tokens_per_query), [`ai_max_output_tokens_per_query`](/operations/settings/settings#ai_max_output_tokens_per_query)), and API calls ([`ai_max_api_calls_per_query`](/operations/settings/settings#ai_max_api_calls_per_query)).
- **Retry with backoff**: Transient failures are retried ([`ai_max_retries`](/operations/settings/settings#ai_max_retries)) with exponential backoff ([`ai_retry_initial_delay_ms`](/operations/settings/settings#ai_retry_initial_delay_ms)).

## Configuration {#configuration}

AI functions reference a **named collection** that stores provider credentials and configuration. The first argument to each function is the name of this collection. If omitted, the [`default_ai_provider`](/operations/settings/settings#default_ai_provider) setting is used.

```sql
CREATE NAMED COLLECTION ai_credentials AS
    provider = 'openai',
    endpoint = 'https://api.openai.com/v1/chat/completions',
    model = 'gpt-4o-mini',
    api_key = 'sk-...';
```

### Named collection parameters {#named-collection-parameters}

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `provider` | String | `'openai'` | Model provider. Supported: `'openai'`, `'anthropic'`. |
| `endpoint` | String | — | API endpoint URL. |
| `model` | String | — | Model name (e.g. `'gpt-4o-mini'`, `'text-embedding-3-small'`). |
| `api_key` | String | — | Authentication key for the provider. |

:::note
Any OpenAI-compatible API (e.g. vLLM, Ollama, LiteLLM) can be used by setting `provider = 'openai'` and pointing the `endpoint` to your service.
:::

### Query-level settings {#query-level-settings}

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| [`default_ai_provider`](/operations/settings/settings#default_ai_provider) | String | `''` | Default named collection used when no collection argument is passed. |
| [`ai_request_timeout_sec`](/operations/settings/settings#ai_request_timeout_sec) | UInt64 | `60` | HTTP timeout per request in seconds. |
| [`ai_max_concurrent_requests`](/operations/settings/settings#ai_max_concurrent_requests) | UInt64 | `16` | Maximum parallel API requests. |
| [`ai_max_rps`](/operations/settings/settings#ai_max_rps) | UInt64 | `50` | Maximum requests per second. |
| [`ai_max_retries`](/operations/settings/settings#ai_max_retries) | UInt64 | `3` | Number of retry attempts on transient failure. |
| [`ai_retry_initial_delay_ms`](/operations/settings/settings#ai_retry_initial_delay_ms) | UInt64 | `1000` | Initial retry delay in milliseconds (doubles on each retry). |
| [`ai_cache_ttl_sec`](/operations/settings/settings#ai_cache_ttl_sec) | UInt64 | `86400` | Cache time-to-live in seconds. Set to `0` to disable caching. |
| [`ai_on_error`](/operations/settings/settings#ai_on_error) | String | `'throw'` | Behavior on API error: `'throw'` raises an exception, `'null'` returns NULL. |
| [`ai_max_rows_per_query`](/operations/settings/settings#ai_max_rows_per_query) | UInt64 | `100000` | Maximum rows processed by AI functions per query. |
| [`ai_max_input_tokens_per_query`](/operations/settings/settings#ai_max_input_tokens_per_query) | UInt64 | `1000000` | Maximum input tokens per query. |
| [`ai_max_output_tokens_per_query`](/operations/settings/settings#ai_max_output_tokens_per_query) | UInt64 | `500000` | Maximum output tokens per query. |
| [`ai_max_api_calls_per_query`](/operations/settings/settings#ai_max_api_calls_per_query) | UInt64 | `1000` | Maximum API calls per query. |
| [`ai_on_quota_exceeded`](/operations/settings/settings#ai_on_quota_exceeded) | String | `'throw'` | Behavior when quota is exceeded: `'throw'` raises an exception, `'null'` returns NULL for remaining rows. |

## aiClassify {#aiclassify}

Classifies input text into one of the provided categories.

**Syntax**

```sql
aiClassify([collection,] text, categories[, temperature])
```

**Arguments**

- `collection`: Name of the named collection. [String](../data-types/string.md). Optional if [`default_ai_provider`](/operations/settings/settings#default_ai_provider) is set.
- `text`: Text to classify. [String](../data-types/string.md).
- `categories`: Array of category labels. [Array(String)](../data-types/array.md).
- `temperature`: Sampling temperature. Default: `0.0`. [Float64](../data-types/float.md). Optional.

**Returned value**

- One of the category strings from the `categories` array. Type: [Nullable(String)](../data-types/nullable.md).

**Example**

```sql
SELECT aiClassify('ai_credentials', 'I absolutely love ClickHouse!', ['positive', 'negative', 'neutral']) AS sentiment;
```

```response
┌─sentiment─┐
│ positive  │
└───────────┘
```

Classify multiple rows:

```sql
SELECT
    review,
    aiClassify('ai_credentials', review, ['positive', 'negative', 'neutral']) AS sentiment
FROM product_reviews
LIMIT 10;
```

## aiExtract {#aiextract}

Extracts information from text.

**Syntax**

```sql
aiExtract([collection,] text, what_to_extract[, temperature])
```

**Arguments**

- `collection`: Name of the named collection. [String](../data-types/string.md). Optional if [`default_ai_provider`](/operations/settings/settings#default_ai_provider) is set.
- `text`: Input text to extract from. [String](../data-types/string.md).
- `what_to_extract`: Description of what to extract, or a JSON template defining the output schema (e.g. `'{"company": "company name", "location": "city"}'`). [String](../data-types/string.md).
- `temperature`: Sampling temperature. Default: `0.0`. [Float64](../data-types/float.md). Optional.

**Returned value**

- The extracted text. Type: [Nullable(String)](../data-types/nullable.md).

**Example**

```sql
SELECT aiExtract('ai_credentials', 'John Doe works at Acme Corp since 2020.', 'company name') AS company;
```

```response
┌─company───┐
│ Acme Corp │
└───────────┘
```

You can use a JSON as an extraction prompt to define the schema you want back.

```sql
SELECT
    JSONExtractString(info, 'company') AS company,
    JSONExtractString(info, 'location') AS location,
    JSONExtractString(info, 'stack') AS stack,
    JSONExtractString(info, 'contact') AS contact,
    JSONExtractString(info, 'remote') AS remote
FROM
(
    SELECT aiExtract(
        'ai_credentials',
        text,
        '{"company": "company name", "location": "city and state or country", "stack": "main technologies, comma-separated", "contact": "email address or application URL if mentioned, or null", "remote": "yes, no, or hybrid"}'
    ) AS info
    FROM default.hackernews
    WHERE parent IN (22665398, 16735011, 15601729)
      AND type = 'comment'
      AND text != ''
      AND length(text) > 100
    ORDER BY cityHash64(id) ASC
    LIMIT 30
    SETTINGS ai_max_rows_per_query = 35, ai_max_rps = 10
)
ORDER BY company ASC;
```

```response
┌─company────────┬─location───────────┬─stack──────────────────────┬─contact─────────────────────────┬─remote─┐
│ Airbnb         │ San Francisco, CA  │ Ruby, React, Java          │ https://careers.airbnb.com      │ hybrid │
│ Datadog        │ New York, NY       │ Go, Python, Kafka          │ jobs@datadoghq.com              │ no     │
│ Fly.io         │ Remote             │ Rust, Go, Elixir           │ https://fly.io/jobs             │ yes    │
│ PlanetScale    │ Remote             │ Go, MySQL, Vitess          │ null                            │ yes    │
│ Stripe         │ San Francisco, CA  │ Ruby, Scala, TypeScript    │ https://stripe.com/jobs         │ hybrid │
└────────────────┴────────────────────┴────────────────────────────┴─────────────────────────────────┴────────┘
```

This works with any JSON schema, you can add or remove keys to control exactly what will be extracted.

## aiTranslate {#aitranslate}

Translates text into the specified target language.

**Syntax**

```sql
aiTranslate([collection,] text, target_language[, instructions][, temperature])
```

**Arguments**

- `collection`: Name of the named collection. [String](../data-types/string.md). Optional if [`default_ai_provider`](/operations/settings/settings#default_ai_provider) is set.
- `text`: Text to translate. [String](../data-types/string.md).
- `target_language`: Target language name (e.g. `'French'`, `'Japanese'`, `'Spanish'`). [String](../data-types/string.md).
- `instructions`: Additional translation instructions (e.g. `'use formal tone'`). [String](../data-types/string.md). Optional.
- `temperature`: Sampling temperature. Default: `0.3`. [Float64](../data-types/float.md). Optional.

**Returned value**

- The translated text. Type: [Nullable(String)](../data-types/nullable.md).

**Example**

```sql
SELECT aiTranslate('ai_credentials', 'Hello, how are you?', 'French') AS translated;
```

```response
┌─translated──────────────────┐
│ Bonjour, comment allez-vous? │
└─────────────────────────────┘
```

Translate a whole column:

```sql
SELECT
    original_text,
    aiTranslate('ai_credentials', original_text, 'German') AS german_text
FROM articles
LIMIT 5;
```

## aiGenerateSQL {#aigeneratesql}

Generates a SQL query from a natural language description using AI. The function automatically discovers the database schema from the ClickHouse catalog, introspecting all accessible databases and tables to build context for AI.

**Syntax**

```sql
aiGenerateSQL([collection,] prompt[, temperature])
```

**Arguments**

- `collection`: Name of the named collection. [String](../data-types/string.md). Optional if [`default_ai_provider`](/operations/settings/settings#default_ai_provider) is set.
- `prompt`: Natural language description of the desired query (e.g. `'Count users by country'`). [String](../data-types/string.md).
- `temperature`: Sampling temperature. Default: `0.1`. [Float64](../data-types/float.md). Optional.

**Returned value**

- A SQL query string. Type: [Nullable(String)](../data-types/nullable.md).

**Example**

```sql
SELECT aiGenerateSQL(
    'ai_credentials',
    'Find the top 5 customers by total order amount'
) AS generated_sql;
```

```response
┌─generated_sql──────────────────────────────────────────────────────────────────────────┐
│ SELECT customer_id, sum(amount) AS total FROM orders GROUP BY customer_id ORDER BY total DESC LIMIT 5 │
└────────────────────────────────────────────────────────────────────────────────────────┘
```

## aiGenerateContent {#aigeneratecontent}

Generates free-form text content from a prompt.

**Syntax**

```sql
aiGenerateContent([collection,] prompt[, system_prompt][, temperature])
```

**Arguments**

- `collection`: Name of the named collection. [String](../data-types/string.md). Optional if [`default_ai_provider`](/operations/settings/settings#default_ai_provider) is set.
- `prompt`: The prompt or question for AI. [String](../data-types/string.md).
- `system_prompt`: System-level instruction for the model. [String](../data-types/string.md). Optional.
- `temperature`: Sampling temperature. Default: `0.7`. [Float64](../data-types/float.md). Optional.

**Returned value**

- The generated text response. Type: [Nullable(String)](../data-types/nullable.md).

**Example**

```sql
SELECT aiGenerateContent('ai_credentials', 'What is 2 + 2? Reply with just the number.') AS answer;
```

```response
┌─answer─┐
│ 4      │
└────────┘
```

Summarize column values:

```sql
SELECT
    article_title,
    aiGenerateContent('ai_credentials', concat('Summarize in one sentence: ', article_body)) AS summary
FROM articles
LIMIT 5;
```

## Supported providers {#supported-providers}

| Provider | `provider` value | Chat functions | Notes |
|----------|-----------------|----------------|-------|
| OpenAI | `'openai'` | Yes | Default provider. |
| Anthropic | `'anthropic'` | Yes | Uses `/v1/messages` endpoint. |
| HuggingFace TEI | `'huggingface'` or `'tei'` | Yes | Uses OpenAI-compatible API format. Useful for self-hosted models. |


## Observability {#observability}

AI function activity is tracked through ClickHouse [ProfileEvents](/operations/system-tables/query_log):

| Event | Description |
|-------|-------------|
| `AIAPICalls` | Number of HTTP requests made to the AI provider. |
| `AIInputTokens` | Total input tokens consumed. |
| `AICacheHits` | Number of results served from cache. |
| `AICacheMisses` | Number of results that required an API call. |
| `AIRowsProcessed` | Number of rows that received a result. |
| `AIRowsSkipped` | Number of rows skipped (NULL, empty, quota exceeded). |

Query these events:

```sql
SELECT
    ProfileEvents['AIAPICalls'] AS api_calls,
    ProfileEvents['AICacheHits'] AS cache_hits,
    ProfileEvents['AIInputTokens'] AS tokens
FROM system.query_log
WHERE query='query_id'
AND type = 'QueryFinish'
ORDER BY event_time DESC;
```
