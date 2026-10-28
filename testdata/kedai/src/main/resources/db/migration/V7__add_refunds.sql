ALTER TABLE orders ADD COLUMN currency char(3) NOT NULL DEFAULT 'MYR';
CREATE TABLE refunds (id bigserial PRIMARY KEY, order_id bigint REFERENCES orders(id), amount numeric(12,2));
