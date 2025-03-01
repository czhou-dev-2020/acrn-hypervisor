import React from "react";

import Form from "react-bootstrap/Form";

import {WidgetProps} from "@rjsf/core";

const RadioWidget = (
    {
        id,
        schema,
        options,
        value,
        required,
        disabled,
        readonly,
        label,
        onChange,
        onBlur,
        onFocus,
        uiSchema,
    }: WidgetProps) => {
    const {enumOptions, enumDisabled} = options;

    const _onChange = ({target: {value},}: React.ChangeEvent<HTMLInputElement>) =>
        onChange(schema.type == "boolean" ? value !== "false" : value);
    const _onBlur = ({target: {value}}: React.FocusEvent<HTMLInputElement>) =>
        onBlur(id, value);
    const _onFocus = ({target: {value},}: React.FocusEvent<HTMLInputElement>) => onFocus(id, value);

    const inline = Boolean(options && options.inline);

    return (
        <Form.Group className="mb-0 row">
            <Form.Label className="col-sm-4 col-form-label">
                {uiSchema["ui:title"] || schema.title || label}
                {(label || uiSchema["ui:title"] || schema.title) && required ? "*" : null}
            </Form.Label>
            <div className="col-sm-8">
                {(enumOptions as any).map((option: any, i: number) => {
                    const itemDisabled =
                        Array.isArray(enumDisabled) &&
                        enumDisabled.indexOf(option.value) !== -1;
                    const checked = option.value == value;

                    // @ts-ignore
                    const radio = (
                        <Form.Check
                            inline={inline}
                            label={option.label}
                            id={option.label}
                            key={i}
                            name={id}
                            type="radio"
                            disabled={disabled || itemDisabled || readonly}
                            checked={checked}
                            required={required}
                            value={option.value}
                            onChange={_onChange}
                            onBlur={_onBlur}
                            onFocus={_onFocus}
                        />
                    );
                    return radio;
                })}
            </div>
        </Form.Group>
    );
};

export default RadioWidget;
